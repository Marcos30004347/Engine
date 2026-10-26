#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "editor/virtualgeometry/FallbackMeshBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/RadixSort2Pass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "softwarebvh/CWBVHBuilder.hpp"
#include "softwarebvh/RBVHBuilder.hpp"
#include "softwarebvh/SWRTFallbackMeshFile.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareBLASSceneBridge.hpp"
#include "softwarebvh/SoftwareRayTraceScene.hpp"
#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"
#include "voxel/svraster/SparseVoxelTree64TLASBridge.hpp"
#include "softwarebvh/SoftwareTLASSceneBridge.hpp"
#include "softwarebvh/rendering/SoftwareBLASGPUFinalizePass.hpp"
#include "softwarebvh/rendering/SoftwareBLASGPUPreparePass.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"
#include "softwarebvh/rendering/SoftwareTLASPass.hpp"
#include "softwarebvh/rendering/SoftwareWorldBLASUpdatePass.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace
{

using BLASManager = softwarebvh::SoftwareBLASManager;
using TLASManager = softwarebvh::SoftwareTLASManager;
using SWRTPass = softwarebvh::rendering::SoftwareRayTracePass;
namespace vulkan = rendering::backend::vulkan;
namespace fs = std::filesystem;

constexpr float kEpsilon = 1.0e-5f;
std::string gTestFilter;

bool isExpectedGpuBackendUnavailable(const std::exception &error)
{
  const std::string message = error.what();
#if defined(__APPLE__)
  if (message.find("failed to create instance") != std::string::npos)
    return true;
#endif
  return message.find("Vulkan is not supported on this device") != std::string::npos;
}

bool floatNear(float actual, float expected, float epsilon = kEpsilon)
{
  return std::fabs(actual - expected) <= epsilon;
}

void expectTrue(bool condition, const std::string &message)
{
  if (!condition)
  {
    std::cerr << "  [FAIL] " << message << "\n";
    throw std::runtime_error(message);
  }
}

void expectEqual(uint32_t actual, uint32_t expected, const std::string &message)
{
  if (actual != expected)
  {
    const std::string full = message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual);
    std::cerr << "  [FAIL] " << full << "\n";
    throw std::runtime_error(full);
  }
}

void expectNear(float actual, float expected, const std::string &message, float epsilon = kEpsilon)
{
  if (!floatNear(actual, expected, epsilon))
  {
    const std::string full = message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual);
    std::cerr << "  [FAIL] " << full << "\n";
    throw std::runtime_error(full);
  }
}

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path teapotAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/teapot.obj";
}

fs::path sponzaAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/sponza.obj";
}

fs::path stanfordBunnyAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/stanford-bunny.obj";
}

fs::path makeTempPath(const std::string &name)
{
  return fs::temp_directory_path() / name;
}

void compareBounds(const virtualgeometry::AABB &actual, const virtualgeometry::AABB &expected, const std::string &label)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    expectNear(actual.minPoint[axis], expected.minPoint[axis], label + " min[" + std::to_string(axis) + "]");
    expectNear(actual.maxPoint[axis], expected.maxPoint[axis], label + " max[" + std::to_string(axis) + "]");
  }
}

math::Vec3f loadPosition(const virtualgeometry::FallbackMeshVertex &vertex)
{
  return math::Vec3f(vertex.position[0], vertex.position[1], vertex.position[2]);
}

math::Vec3f loadArray3(const float value[3])
{
  return math::Vec3f(value[0], value[1], value[2]);
}

virtualgeometry::FallbackMeshData loadObjFallbackMesh(const fs::path &assetPath)
{
  expectTrue(fs::exists(assetPath), "OBJ fallback asset should exist: " + assetPath.string());

  const fs::path tempRoot = fs::temp_directory_path() / ("SoftwareBVHRuntimeTests_" + assetPath.stem().string());
  const fs::path materialsDir = tempRoot / "materials";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(materialsDir, error);
  expectTrue(!error, "Failed to create temporary material directory");

  const virtualgeometry::VirtualGeometrySourceAsset asset = virtualgeometry::VirtualGeometryEncoder::loadAssetFromOBJFile(assetPath.string(), materialsDir.string());
  expectTrue(!asset.vertices.empty(), "Loaded OBJ asset should contain vertices");
  expectTrue(!asset.shapes.empty(), "Loaded OBJ asset should contain shapes");

  const virtualgeometry::FallbackMeshData mesh = virtualgeometry::FallbackMeshBuilder::buildOriginal(asset);
  fs::remove_all(tempRoot, error);
  return mesh;
}

virtualgeometry::VirtualGeometrySourceAsset loadObjSourceAssetForTest(const fs::path &assetPath, const std::string &testName)
{
  expectTrue(fs::exists(assetPath), "OBJ source asset should exist: " + assetPath.string());

  const fs::path tempRoot = fs::temp_directory_path() / ("SoftwareBVHRuntimeTests_" + testName);
  const fs::path materialsDir = tempRoot / "materials";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(materialsDir, error);
  expectTrue(!error, "Failed to create temporary material directory for " + testName);

  virtualgeometry::VirtualGeometrySourceAsset asset = virtualgeometry::VirtualGeometryEncoder::loadAssetFromOBJFile(assetPath.string(), materialsDir.string());
  expectTrue(!asset.vertices.empty(), "Loaded OBJ asset should contain vertices: " + assetPath.string());
  expectTrue(!asset.shapes.empty(), "Loaded OBJ asset should contain shapes: " + assetPath.string());

  fs::remove_all(tempRoot, error);
  return asset;
}

struct TriangleRayTarget
{
  math::Vec3f centroid = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f normal = math::Vec3f(0.0f, 0.0f, 1.0f);
  math::Vec3f cameraPosition = math::Vec3f(0.0f, 0.0f, 0.0f);
  uint32_t primitiveIndex = 0u;
};

bool intersectTriangleRay(const math::Vec3f &rayOrigin, const math::Vec3f &rayDirection, const math::Vec3f &a, const math::Vec3f &b, const math::Vec3f &c, float *outT)
{
  const math::Vec3f ab = b - a;
  const math::Vec3f ac = c - a;
  const math::Vec3f p = rayDirection.cross(ac);
  const float det = ab.dot(p);
  if (std::abs(det) <= 1.0e-8f)
    return false;

  const float invDet = 1.0f / det;
  const math::Vec3f t = rayOrigin - a;
  const float u = t.dot(p) * invDet;
  if (u < 0.0f || u > 1.0f)
    return false;

  const math::Vec3f q = t.cross(ab);
  const float v = rayDirection.dot(q) * invDet;
  if (v < 0.0f || (u + v) > 1.0f)
    return false;

  const float hitT = ac.dot(q) * invDet;
  if (hitT <= 1.0e-4f)
    return false;

  *outT = hitT;
  return true;
}

uint32_t findNearestHitPrimitive(const virtualgeometry::FallbackMeshData &mesh, const math::Vec3f &rayOrigin, const math::Vec3f &rayDirection)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  float bestT = std::numeric_limits<float>::infinity();
  uint32_t bestPrimitive = BLASManager::INVALID_PRIMITIVE_INDEX;
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    const uint32_t baseIndex = primitiveIndex * 3u;
    const math::Vec3f a = loadPosition(mesh.vertices[mesh.indices[baseIndex + 0u]]);
    const math::Vec3f b = loadPosition(mesh.vertices[mesh.indices[baseIndex + 1u]]);
    const math::Vec3f c = loadPosition(mesh.vertices[mesh.indices[baseIndex + 2u]]);
    float hitT = 0.0f;
    if (intersectTriangleRay(rayOrigin, rayDirection, a, b, c, &hitT) && hitT < bestT)
    {
      bestT = hitT;
      bestPrimitive = primitiveIndex;
    }
  }
  return bestPrimitive;
}

std::vector<TriangleRayTarget> selectExteriorTraceTargetTriangles(const virtualgeometry::FallbackMeshData &mesh, uint32_t maxTargets)
{
  expectTrue(mesh.indices.size() >= 3u, "Trace target mesh should contain at least one triangle");
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  const math::Vec3f meshCenter(0.5f * (mesh.boundsMin[0] + mesh.boundsMax[0]), 0.5f * (mesh.boundsMin[1] + mesh.boundsMax[1]), 0.5f * (mesh.boundsMin[2] + mesh.boundsMax[2]));
  const float cameraDistance = std::max((math::Vec3f(mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]) - math::Vec3f(mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2])).length() * 0.35f, 1.5f);

  std::vector<TriangleRayTarget> targets;
  targets.reserve(maxTargets);
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount && targets.size() < maxTargets; ++primitiveIndex)
  {
    const uint32_t baseIndex = primitiveIndex * 3u;
    const math::Vec3f a = loadPosition(mesh.vertices[mesh.indices[baseIndex + 0u]]);
    const math::Vec3f b = loadPosition(mesh.vertices[mesh.indices[baseIndex + 1u]]);
    const math::Vec3f c = loadPosition(mesh.vertices[mesh.indices[baseIndex + 2u]]);
    const math::Vec3f geometricNormal = (b - a).cross(c - a);
    if (geometricNormal.length() <= 1.0e-6f)
      continue;

    const math::Vec3f normal = geometricNormal.normalize();
    const math::Vec3f centroid = (a + b + c) * (1.0f / 3.0f);
    if (normal.dot(centroid - meshCenter) <= 0.0f)
      continue;

    const math::Vec3f cameraPosition = centroid + normal * cameraDistance;
    const math::Vec3f cameraForward = (centroid - cameraPosition).normalize();
    if (findNearestHitPrimitive(mesh, cameraPosition, cameraForward) != primitiveIndex)
      continue;

    targets.push_back(
        TriangleRayTarget{
          .centroid = centroid,
          .normal = normal,
          .cameraPosition = cameraPosition,
          .primitiveIndex = primitiveIndex,
        });
  }

  return targets;
}

TriangleRayTarget makePrimitiveTraceTarget(const virtualgeometry::FallbackMeshData &mesh, uint32_t primitiveIndex, float cameraDistance)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  expectTrue(primitiveIndex < primitiveCount, "Primitive trace target index should be in range");

  const uint32_t baseIndex = primitiveIndex * 3u;
  const math::Vec3f a = loadPosition(mesh.vertices[mesh.indices[baseIndex + 0u]]);
  const math::Vec3f b = loadPosition(mesh.vertices[mesh.indices[baseIndex + 1u]]);
  const math::Vec3f c = loadPosition(mesh.vertices[mesh.indices[baseIndex + 2u]]);
  const math::Vec3f geometricNormal = (b - a).cross(c - a);
  expectTrue(geometricNormal.length() > 1.0e-6f, "Primitive trace target triangle should be non-degenerate");

  const math::Vec3f normal = geometricNormal.normalize();
  const math::Vec3f centroid = (a + b + c) * (1.0f / 3.0f);
  const math::Vec3f cameraPosition = centroid + normal * cameraDistance;
  const math::Vec3f cameraForward = (centroid - cameraPosition).normalize();
  expectEqual(findNearestHitPrimitive(mesh, cameraPosition, cameraForward), primitiveIndex, "Primitive trace target CPU nearest hit should match the requested primitive");

  return TriangleRayTarget{
    .centroid = centroid,
    .normal = normal,
    .cameraPosition = cameraPosition,
    .primitiveIndex = primitiveIndex,
  };
}

void makeCameraBasis(const math::Vec3f &forward, math::Vec3f *outRight, math::Vec3f *outUp)
{
  const math::Vec3f forwardNorm = forward.normalize();
  const math::Vec3f upSeed = std::abs(forwardNorm[2]) < 0.999f ? math::Vec3f(0.0f, 0.0f, 1.0f) : math::Vec3f(0.0f, 1.0f, 0.0f);
  *outRight = upSeed.cross(forwardNorm).normalize();
  *outUp = forwardNorm.cross(*outRight).normalize();
}

template <typename T> std::vector<T> readBufferVector(rendering::RenderGraph &renderGraph, const rendering::Buffer &buffer, uint32_t count, uint64_t offset = 0u)
{
  std::vector<T> values(count);
  if (count == 0u)
    return values;

  renderGraph.bufferRead(
      buffer,
      offset,
      static_cast<uint64_t>(count) * sizeof(T),
      [&](const void *data)
      {
        std::memcpy(values.data(), data, values.size() * sizeof(T));
      });
  return values;
}

template <typename T> T readBufferValue(rendering::RenderGraph &renderGraph, const rendering::Buffer &buffer, uint64_t offset = 0u)
{
  const std::vector<T> values = readBufferVector<T>(renderGraph, buffer, 1u, offset);
  return values.front();
}

void runGraph(rendering::RenderGraph &renderGraph)
{
  renderGraph.compile();
  rendering::RenderGraph::Frame frame{};
  rendering::RenderGraph::Overrides overrides{};
  renderGraph.run(frame, overrides);
  renderGraph.waitFrame(frame);
}

void runGraphWithSortTimers(rendering::RenderGraph &renderGraph, const std::vector<rendering::Pass *> &passes)
{
  renderGraph.compile();
  for (rendering::Pass *pass : passes)
  {
    auto *sortPass = dynamic_cast<rendering::gpgpu::RadixSort2Pass<uint32_t, uint32_t> *>(pass);
    if (sortPass != nullptr)
    {
      sortPass->ensureDispatchTimerAllocated();
    }
  }

  rendering::RenderGraph::Frame frame{};
  rendering::RenderGraph::Overrides overrides{};
  renderGraph.run(frame, overrides);
  renderGraph.waitFrame(frame);
}

virtualgeometry::FallbackMeshData makeFrontFacingTriangleMesh(float z)
{
  virtualgeometry::FallbackMeshData mesh{};
  mesh.vertices.resize(3u);
  mesh.indices = {0u, 1u, 2u};
  mesh.sections = {virtualgeometry::FallbackMeshSection{.indexOffset = 0u, .indexCount = 3u, .materialIndex = 0u}};
  mesh.materialFiles = {"materials/front_triangle.vmat"};

  mesh.vertices[0u].position[0] = -0.5f;
  mesh.vertices[0u].position[1] = -0.5f;
  mesh.vertices[0u].position[2] = z;
  mesh.vertices[1u].position[0] = 0.5f;
  mesh.vertices[1u].position[1] = -0.5f;
  mesh.vertices[1u].position[2] = z;
  mesh.vertices[2u].position[0] = 0.0f;
  mesh.vertices[2u].position[1] = 0.5f;
  mesh.vertices[2u].position[2] = z;

  for (auto &vertex : mesh.vertices)
  {
    vertex.normal[2] = 1.0f;
  }

  mesh.boundsMin[0] = -0.5f;
  mesh.boundsMin[1] = -0.5f;
  mesh.boundsMin[2] = z;
  mesh.boundsMax[0] = 0.5f;
  mesh.boundsMax[1] = 0.5f;
  mesh.boundsMax[2] = z;
  return mesh;
}

virtualgeometry::FallbackMeshData translateMesh(virtualgeometry::FallbackMeshData mesh, const math::Vec3f &translation)
{
  for (virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    vertex.position[0] += translation[0];
    vertex.position[1] += translation[1];
    vertex.position[2] += translation[2];
  }

  mesh.boundsMin[0] += translation[0];
  mesh.boundsMin[1] += translation[1];
  mesh.boundsMin[2] += translation[2];
  mesh.boundsMax[0] += translation[0];
  mesh.boundsMax[1] += translation[1];
  mesh.boundsMax[2] += translation[2];
  return mesh;
}

void compareFallbackMeshData(const virtualgeometry::FallbackMeshData &actual, const virtualgeometry::FallbackMeshData &expected, const std::string &label)
{
  expectEqual(actual.lodLevel, expected.lodLevel, label + " lod level");
  expectNear(actual.targetError, expected.targetError, label + " target error");
  expectNear(actual.relativeError, expected.relativeError, label + " relative error");
  virtualgeometry::AABB actualBounds{};
  virtualgeometry::AABB expectedBounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    actualBounds.minPoint[axis] = actual.boundsMin[axis];
    actualBounds.maxPoint[axis] = actual.boundsMax[axis];
    expectedBounds.minPoint[axis] = expected.boundsMin[axis];
    expectedBounds.maxPoint[axis] = expected.boundsMax[axis];
  }
  compareBounds(actualBounds, expectedBounds, label + " bounds");

  expectEqual(static_cast<uint32_t>(actual.vertices.size()), static_cast<uint32_t>(expected.vertices.size()), label + " vertex count");
  expectEqual(static_cast<uint32_t>(actual.indices.size()), static_cast<uint32_t>(expected.indices.size()), label + " index count");
  expectEqual(static_cast<uint32_t>(actual.sections.size()), static_cast<uint32_t>(expected.sections.size()), label + " section count");
  expectEqual(static_cast<uint32_t>(actual.materialFiles.size()), static_cast<uint32_t>(expected.materialFiles.size()), label + " material file count");

  for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(actual.vertices.size()); ++vertexIndex)
  {
    const auto &actualVertex = actual.vertices[vertexIndex];
    const auto &expectedVertex = expected.vertices[vertexIndex];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      expectNear(actualVertex.position[axis], expectedVertex.position[axis], label + " vertex position[" + std::to_string(vertexIndex) + "][" + std::to_string(axis) + "]");
      expectNear(actualVertex.normal[axis], expectedVertex.normal[axis], label + " vertex normal[" + std::to_string(vertexIndex) + "][" + std::to_string(axis) + "]");
    }
    for (uint32_t axis = 0u; axis < 2u; ++axis)
      expectNear(actualVertex.uv[axis], expectedVertex.uv[axis], label + " vertex uv[" + std::to_string(vertexIndex) + "][" + std::to_string(axis) + "]");
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
      expectEqual(actualVertex.boneIndices[influence], expectedVertex.boneIndices[influence], label + " vertex bone index[" + std::to_string(vertexIndex) + "][" + std::to_string(influence) + "]");
      expectNear(actualVertex.boneWeights[influence], expectedVertex.boneWeights[influence], label + " vertex bone weight[" + std::to_string(vertexIndex) + "][" + std::to_string(influence) + "]");
    }
  }

  for (uint32_t index = 0u; index < static_cast<uint32_t>(actual.indices.size()); ++index)
    expectEqual(actual.indices[index], expected.indices[index], label + " index[" + std::to_string(index) + "]");

  for (uint32_t sectionIndex = 0u; sectionIndex < static_cast<uint32_t>(actual.sections.size()); ++sectionIndex)
  {
    expectEqual(actual.sections[sectionIndex].indexOffset, expected.sections[sectionIndex].indexOffset, label + " section index offset[" + std::to_string(sectionIndex) + "]");
    expectEqual(actual.sections[sectionIndex].indexCount, expected.sections[sectionIndex].indexCount, label + " section index count[" + std::to_string(sectionIndex) + "]");
    expectEqual(actual.sections[sectionIndex].materialIndex, expected.sections[sectionIndex].materialIndex, label + " section material[" + std::to_string(sectionIndex) + "]");
  }

  for (uint32_t materialIndex = 0u; materialIndex < static_cast<uint32_t>(actual.materialFiles.size()); ++materialIndex)
    expectTrue(actual.materialFiles[materialIndex] == expected.materialFiles[materialIndex], label + " material file[" + std::to_string(materialIndex) + "]");

  expectTrue(actual.indices.size() % 3u == 0u, label + " index count should describe whole triangles");
  for (uint32_t triangleIndex = 0u; triangleIndex < static_cast<uint32_t>(actual.indices.size() / 3u); ++triangleIndex)
  {
    for (uint32_t corner = 0u; corner < 3u; ++corner)
    {
      const uint32_t indexOffset = triangleIndex * 3u + corner;
      const uint32_t actualVertexIndex = actual.indices[indexOffset];
      const uint32_t expectedVertexIndex = expected.indices[indexOffset];
      expectTrue(actualVertexIndex < actual.vertices.size(), label + " triangle actual vertex index in range");
      expectTrue(expectedVertexIndex < expected.vertices.size(), label + " triangle expected vertex index in range");
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        expectNear(
            actual.vertices[actualVertexIndex].position[axis],
            expected.vertices[expectedVertexIndex].position[axis],
            label + " triangle position[" + std::to_string(triangleIndex) + "][" + std::to_string(corner) + "][" + std::to_string(axis) + "]");
      }
    }
  }
}

void compareCwbvhBuildResult(const softwarebvh::CWBVHBuilder::BuildResult &actual, const softwarebvh::CWBVHBuilder::BuildResult &expected, const std::string &label)
{
  expectEqual(actual.primitiveCount, expected.primitiveCount, label + " primitive count");
  expectEqual(actual.rootNodeIndex, expected.rootNodeIndex, label + " root node index");
  compareBounds(actual.localBounds, expected.localBounds, label + " local bounds");
  expectEqual(static_cast<uint32_t>(actual.primitiveRemap.size()), static_cast<uint32_t>(expected.primitiveRemap.size()), label + " primitive remap count");
  expectEqual(static_cast<uint32_t>(actual.triangleTransforms.size()), static_cast<uint32_t>(expected.triangleTransforms.size()), label + " triangle transform count");
  expectEqual(static_cast<uint32_t>(actual.canonicalNodes.size()), static_cast<uint32_t>(expected.canonicalNodes.size()), label + " canonical node count");

  for (uint32_t primitiveIndex = 0u; primitiveIndex < static_cast<uint32_t>(actual.primitiveRemap.size()); ++primitiveIndex)
    expectEqual(actual.primitiveRemap[primitiveIndex], expected.primitiveRemap[primitiveIndex], label + " primitive remap[" + std::to_string(primitiveIndex) + "]");

  for (uint32_t transformIndex = 0u; transformIndex < static_cast<uint32_t>(actual.triangleTransforms.size()); ++transformIndex)
  {
    for (uint32_t element = 0u; element < 4u; ++element)
    {
      expectNear(
          actual.triangleTransforms[transformIndex].row0[element],
          expected.triangleTransforms[transformIndex].row0[element],
          label + " triangle transform row0[" + std::to_string(transformIndex) + "][" + std::to_string(element) + "]");
      expectNear(
          actual.triangleTransforms[transformIndex].row1[element],
          expected.triangleTransforms[transformIndex].row1[element],
          label + " triangle transform row1[" + std::to_string(transformIndex) + "][" + std::to_string(element) + "]");
      expectNear(
          actual.triangleTransforms[transformIndex].row2[element],
          expected.triangleTransforms[transformIndex].row2[element],
          label + " triangle transform row2[" + std::to_string(transformIndex) + "][" + std::to_string(element) + "]");
    }
  }

  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(actual.canonicalNodes.size()); ++nodeIndex)
  {
    const auto &actualNode = actual.canonicalNodes[nodeIndex];
    const auto &expectedNode = expected.canonicalNodes[nodeIndex];
    expectEqual(actualNode.parent, expectedNode.parent, label + " canonical parent[" + std::to_string(nodeIndex) + "]");
    expectEqual(actualNode.left, expectedNode.left, label + " canonical left[" + std::to_string(nodeIndex) + "]");
    expectEqual(actualNode.right, expectedNode.right, label + " canonical right[" + std::to_string(nodeIndex) + "]");
    expectEqual(actualNode.primitiveIndex, expectedNode.primitiveIndex, label + " canonical primitive[" + std::to_string(nodeIndex) + "]");
    expectEqual(actualNode.subtreePrimitiveCount, expectedNode.subtreePrimitiveCount, label + " canonical subtree primitive count[" + std::to_string(nodeIndex) + "]");
    expectEqual(actualNode.isLeaf ? 1u : 0u, expectedNode.isLeaf ? 1u : 0u, label + " canonical leaf flag[" + std::to_string(nodeIndex) + "]");
    compareBounds(actualNode.bounds, expectedNode.bounds, label + " canonical bounds[" + std::to_string(nodeIndex) + "]");
  }
}

void verifyRtBuildCoversAllTriangles(const softwarebvh::CWBVHBuilder::BuildResult &build, const virtualgeometry::FallbackMeshData &mesh, const std::string &label)
{
  const uint32_t triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  expectEqual(build.primitiveCount, triangleCount, label + " primitive count");
  expectEqual(static_cast<uint32_t>(build.primitiveRemap.size()), triangleCount, label + " primitive remap count");
  expectEqual(static_cast<uint32_t>(build.triangleTransforms.size()), triangleCount, label + " triangle transform count");
  virtualgeometry::AABB meshLocalBounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    meshLocalBounds.minPoint[axis] = mesh.boundsMin[axis];
    meshLocalBounds.maxPoint[axis] = mesh.boundsMax[axis];
  }
  compareBounds(build.localBounds, meshLocalBounds, label + " local bounds");

  std::vector<uint8_t> seen(triangleCount, 0u);
  for (uint32_t remapIndex = 0u; remapIndex < static_cast<uint32_t>(build.primitiveRemap.size()); ++remapIndex)
  {
    const uint32_t primitiveIndex = build.primitiveRemap[remapIndex];
    expectTrue(primitiveIndex < triangleCount, label + " primitive remap entry in range");
    seen[primitiveIndex] = 1u;
  }

  for (uint32_t primitiveIndex = 0u; primitiveIndex < triangleCount; ++primitiveIndex)
    expectEqual(seen[primitiveIndex], 1u, label + " primitive remap covers triangle[" + std::to_string(primitiveIndex) + "]");
}

virtualgeometry::VirtualGeometrySourceAsset makeMultiShapeSourceAsset()
{
  virtualgeometry::VirtualGeometrySourceAsset asset{};
  asset.materialFiles = {
    "materials/shape0.vmat",
    "materials/shape1.vmat",
    "materials/shape2.vmat",
    "materials/shape3.vmat",
  };

  const auto addVertex = [&asset](const float x, const float y, const float z, const float u, const float v) -> uint32_t
  {
    virtualgeometry::Vertex vertex(x, y, z);
    vertex.norm[0] = 0.0f;
    vertex.norm[1] = 0.0f;
    vertex.norm[2] = 1.0f;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    asset.vertices.push_back(vertex);
    return static_cast<uint32_t>(asset.vertices.size() - 1u);
  };

  for (uint32_t shapeIndex = 0u; shapeIndex < 4u; ++shapeIndex)
  {
    virtualgeometry::SourceShapeBuildInput shape{};
    shape.materialIndex = shapeIndex;
    virtualgeometry::MeshPart meshPart{};

    const uint32_t triangleCount = shapeIndex + 1u;
    const float xBase = static_cast<float>(shapeIndex) * 8.0f;
    for (uint32_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
    {
      const float yBase = static_cast<float>(triangleIndex) * 1.75f;
      const uint32_t i0 = addVertex(xBase + 0.0f, yBase + 0.0f, 0.25f * static_cast<float>(shapeIndex), 0.0f, 0.0f);
      const uint32_t i1 = addVertex(xBase + 1.0f, yBase + 0.0f, 0.25f * static_cast<float>(shapeIndex), 1.0f, 0.0f);
      const uint32_t i2 = addVertex(xBase + 0.25f, yBase + 1.0f, 0.25f * static_cast<float>(shapeIndex), 0.25f, 1.0f);
      meshPart.shape.indices.push_back(i0);
      meshPart.shape.indices.push_back(i1);
      meshPart.shape.indices.push_back(i2);
    }

    shape.meshParts.push_back(std::move(meshPart));
    asset.shapes.push_back(std::move(shape));
  }

  return asset;
}

virtualgeometry::FallbackMeshData makeSeparatedTrianglesMesh()
{
  virtualgeometry::FallbackMeshData mesh{};
  mesh.vertices.resize(6u);
  mesh.indices = {0u, 1u, 2u, 3u, 4u, 5u};
  mesh.sections = {virtualgeometry::FallbackMeshSection{.indexOffset = 0u, .indexCount = 6u, .materialIndex = 0u}};
  mesh.materialFiles = {"materials/separated_triangles.vmat"};

  const std::array<std::array<float, 3>, 6> positions{{
    {{-3.0f, 0.0f, 0.0f}},
    {{-2.0f, 0.0f, 0.0f}},
    {{-2.5f, 1.0f, 0.0f}},
    {{2.0f, 0.0f, 0.0f}},
    {{3.0f, 0.0f, 0.0f}},
    {{2.5f, 1.0f, 0.0f}},
  }};

  for (uint32_t vertexIndex = 0u; vertexIndex < mesh.vertices.size(); ++vertexIndex)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      mesh.vertices[vertexIndex].position[axis] = positions[vertexIndex][axis];
    mesh.vertices[vertexIndex].normal[2] = 1.0f;
  }

  mesh.boundsMin[0] = -3.0f;
  mesh.boundsMin[1] = 0.0f;
  mesh.boundsMin[2] = 0.0f;
  mesh.boundsMax[0] = 3.0f;
  mesh.boundsMax[1] = 1.0f;
  mesh.boundsMax[2] = 0.0f;
  return mesh;
}

std::array<math::Vec3f, 9u> makeLeafBeforeInternalChildTranslations()
{
  return {
    math::Vec3f(-64.0f, -64.0f, -64.0f),
    math::Vec3f(16.0f, 16.0f, 16.0f),
    math::Vec3f(17.0f, 16.0f, 16.25f),
    math::Vec3f(18.0f, 16.5f, 16.5f),
    math::Vec3f(19.0f, 17.0f, 16.75f),
    math::Vec3f(20.0f, 17.5f, 17.0f),
    math::Vec3f(21.0f, 18.0f, 17.25f),
    math::Vec3f(22.0f, 18.5f, 17.5f),
    math::Vec3f(22.5f, 19.0f, 17.75f),
  };
}

virtualgeometry::FallbackMeshData makeLeafBeforeInternalChildTrianglesMesh()
{
  virtualgeometry::FallbackMeshData mesh{};
  const auto translations = makeLeafBeforeInternalChildTranslations();
  mesh.vertices.resize(translations.size() * 3u);
  mesh.indices.reserve(translations.size() * 3u);
  mesh.sections = {virtualgeometry::FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(translations.size() * 3u), .materialIndex = 0u}};
  mesh.materialFiles = {"materials/leaf_before_internal_triangles.vmat"};

  for (uint32_t triangleIndex = 0u; triangleIndex < static_cast<uint32_t>(translations.size()); ++triangleIndex)
  {
    const math::Vec3f center = translations[triangleIndex];
    const uint32_t vertexBase = triangleIndex * 3u;
    const std::array<math::Vec3f, 3u> positions = {
      center + math::Vec3f(-0.25f, -0.25f, 0.0f),
      center + math::Vec3f(+0.25f, -0.25f, 0.0f),
      center + math::Vec3f(0.0f, +0.25f, 0.0f),
    };

    for (uint32_t corner = 0u; corner < 3u; ++corner)
    {
      virtualgeometry::FallbackMeshVertex &vertex = mesh.vertices[vertexBase + corner];
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        vertex.position[axis] = positions[corner][axis];
      vertex.normal[2] = 1.0f;
      mesh.indices.push_back(vertexBase + corner);
    }
  }

  expectTrue(!mesh.vertices.empty(), "leaf-before-internal triangle mesh should contain vertices");
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    mesh.boundsMin[axis] = mesh.vertices.front().position[axis];
    mesh.boundsMax[axis] = mesh.vertices.front().position[axis];
  }
  for (const virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], vertex.position[axis]);
      mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], vertex.position[axis]);
    }
  }
  return mesh;
}

virtualgeometry::FallbackMeshData makeUnitCubeMesh()
{
  virtualgeometry::FallbackMeshData mesh{};
  mesh.vertices.resize(8u);
  mesh.indices = {
    0u, 1u, 2u, 2u, 1u, 3u, 4u, 6u, 5u, 6u, 7u, 5u, 0u, 2u, 4u, 4u, 2u, 6u, 1u, 5u, 3u, 3u, 5u, 7u, 0u, 4u, 1u, 1u, 4u, 5u, 2u, 3u, 6u, 6u, 3u, 7u,
  };
  mesh.sections = {virtualgeometry::FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(mesh.indices.size()), .materialIndex = 0u}};
  mesh.materialFiles = {"materials/unit_cube.vmat"};

  const std::array<std::array<float, 3>, 8> positions{{
    {{-0.5f, -0.5f, -0.5f}},
    {{0.5f, -0.5f, -0.5f}},
    {{-0.5f, 0.5f, -0.5f}},
    {{0.5f, 0.5f, -0.5f}},
    {{-0.5f, -0.5f, 0.5f}},
    {{0.5f, -0.5f, 0.5f}},
    {{-0.5f, 0.5f, 0.5f}},
    {{0.5f, 0.5f, 0.5f}},
  }};

  for (uint32_t vertexIndex = 0u; vertexIndex < mesh.vertices.size(); ++vertexIndex)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      mesh.vertices[vertexIndex].position[axis] = positions[vertexIndex][axis];
    mesh.vertices[vertexIndex].normal[2] = 1.0f;
  }

  mesh.boundsMin[0] = -0.5f;
  mesh.boundsMin[1] = -0.5f;
  mesh.boundsMin[2] = -0.5f;
  mesh.boundsMax[0] = 0.5f;
  mesh.boundsMax[1] = 0.5f;
  mesh.boundsMax[2] = 0.5f;
  return mesh;
}

virtualgeometry::AABB meshBounds(const virtualgeometry::FallbackMeshData &mesh)
{
  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = mesh.boundsMin[axis];
    bounds.maxPoint[axis] = mesh.boundsMax[axis];
  }
  return bounds;
}

bool isValidTestBounds(const virtualgeometry::AABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(bounds.minPoint[axis]) || !std::isfinite(bounds.maxPoint[axis]))
      return false;
    if (bounds.minPoint[axis] > bounds.maxPoint[axis])
      return false;
  }
  return true;
}

virtualgeometry::AABB translateBounds(const virtualgeometry::AABB &localBounds, const math::Vec3f &translation)
{
  virtualgeometry::AABB worldBounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    worldBounds.minPoint[axis] = localBounds.minPoint[axis] + translation[axis];
    worldBounds.maxPoint[axis] = localBounds.maxPoint[axis] + translation[axis];
  }
  return worldBounds;
}

math::Mat4f makeTranslation(const math::Vec3f &translation)
{
  return math::Mat4f::translation(translation);
}

virtualgeometry::AABB triangleBounds(const virtualgeometry::FallbackMeshData &mesh, uint32_t primitiveIndex)
{
  const uint32_t baseIndex = primitiveIndex * 3u;
  expectTrue(baseIndex + 2u < mesh.indices.size(), "triangleBounds primitive index out of range");

  virtualgeometry::AABB bounds{};
  const uint32_t firstVertex = mesh.indices[baseIndex];
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = mesh.vertices[firstVertex].position[axis];
    bounds.maxPoint[axis] = mesh.vertices[firstVertex].position[axis];
  }

  for (uint32_t corner = 1u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[baseIndex + corner];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      bounds.minPoint[axis] = std::min(bounds.minPoint[axis], mesh.vertices[vertexIndex].position[axis]);
      bounds.maxPoint[axis] = std::max(bounds.maxPoint[axis], mesh.vertices[vertexIndex].position[axis]);
    }
  }

  return bounds;
}

template <typename CWBVHNode> virtualgeometry::AABB decodePackedChildBounds(const CWBVHNode &node, uint32_t childSlot)
{
  expectTrue(childSlot < 8u, "Packed CWBVH child slot should be < 8");
  const uint32_t arrayIndex = childSlot >> 2u;
  const uint32_t bitOffset = (childSlot & 3u) * 8u;

  const auto decodeSignedByte = [](uint32_t word, uint32_t byteIndex) -> int32_t
  {
    return static_cast<int32_t>(static_cast<int8_t>((word >> (byteIndex * 8u)) & 0xffu));
  };
  const auto wordAsFloat = [](uint32_t word) -> float
  {
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
  };

  const float parentMinX = wordAsFloat(node.orxy[0]);
  const float parentMinY = wordAsFloat(node.orxy[1]);
  const float parentMinZ = wordAsFloat(node.oeim[0]);
  const float scaleX = std::exp2(static_cast<float>(decodeSignedByte(node.oeim[1], 0u)));
  const float scaleY = std::exp2(static_cast<float>(decodeSignedByte(node.oeim[1], 1u)));
  const float scaleZ = std::exp2(static_cast<float>(decodeSignedByte(node.oeim[1], 2u)));

  const uint32_t qloX = (node.qlox[arrayIndex] >> bitOffset) & 0xffu;
  const uint32_t qloY = (node.qloy[arrayIndex] >> bitOffset) & 0xffu;
  const uint32_t qloZ = (node.qloz[arrayIndex] >> bitOffset) & 0xffu;
  const uint32_t qhiX = (node.qhix[arrayIndex] >> bitOffset) & 0xffu;
  const uint32_t qhiY = (node.qhiy[arrayIndex] >> bitOffset) & 0xffu;
  const uint32_t qhiZ = (node.qhiz[arrayIndex] >> bitOffset) & 0xffu;

  virtualgeometry::AABB bounds{};
  bounds.minPoint[0] = parentMinX + static_cast<float>(qloX) * scaleX;
  bounds.minPoint[1] = parentMinY + static_cast<float>(qloY) * scaleY;
  bounds.minPoint[2] = parentMinZ + static_cast<float>(qloZ) * scaleZ;
  bounds.maxPoint[0] = parentMinX + static_cast<float>(qhiX) * scaleX;
  bounds.maxPoint[1] = parentMinY + static_cast<float>(qhiY) * scaleY;
  bounds.maxPoint[2] = parentMinZ + static_cast<float>(qhiZ) * scaleZ;
  return bounds;
}

template <typename CWBVHNode> virtualgeometry::AABB decodePackedChildBoundsAsShaderVec4s(const CWBVHNode &node, uint32_t childSlot)
{
  expectTrue(childSlot < 8u, "Shader-view packed CWBVH child slot should be < 8");

  std::array<uint32_t, 20u> words{};
  static_assert(sizeof(words) == sizeof(CWBVHNode), "Packed CWBVH node should be exactly five vec4<u32> values");
  std::memcpy(words.data(), &node, sizeof(node));

  const auto decodeSignedByte = [](uint32_t word, uint32_t byteIndex) -> int32_t
  {
    return static_cast<int32_t>(static_cast<int8_t>((word >> (byteIndex * 8u)) & 0xffu));
  };
  const auto wordAsFloat = [](uint32_t word) -> float
  {
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
  };
  const auto byteAt = [](uint32_t word, uint32_t byteIndex) -> uint32_t
  {
    return (word >> (byteIndex * 8u)) & 0xffu;
  };

  const bool secondGroup = childSlot >= 4u;
  const uint32_t byteIndex = childSlot & 3u;

  const uint32_t qloX = byteAt(secondGroup ? words[9u] : words[8u], byteIndex);
  const uint32_t qloY = byteAt(secondGroup ? words[11u] : words[10u], byteIndex);
  const uint32_t qloZ = byteAt(secondGroup ? words[13u] : words[12u], byteIndex);
  const uint32_t qhiX = byteAt(secondGroup ? words[15u] : words[14u], byteIndex);
  const uint32_t qhiY = byteAt(secondGroup ? words[17u] : words[16u], byteIndex);
  const uint32_t qhiZ = byteAt(secondGroup ? words[19u] : words[18u], byteIndex);

  const float parentMinX = wordAsFloat(words[0u]);
  const float parentMinY = wordAsFloat(words[1u]);
  const float parentMinZ = wordAsFloat(words[2u]);
  const float scaleX = std::exp2(static_cast<float>(decodeSignedByte(words[3u], 0u)));
  const float scaleY = std::exp2(static_cast<float>(decodeSignedByte(words[3u], 1u)));
  const float scaleZ = std::exp2(static_cast<float>(decodeSignedByte(words[3u], 2u)));

  virtualgeometry::AABB bounds{};
  bounds.minPoint[0] = parentMinX + static_cast<float>(qloX) * scaleX;
  bounds.minPoint[1] = parentMinY + static_cast<float>(qloY) * scaleY;
  bounds.minPoint[2] = parentMinZ + static_cast<float>(qloZ) * scaleZ;
  bounds.maxPoint[0] = parentMinX + static_cast<float>(qhiX) * scaleX;
  bounds.maxPoint[1] = parentMinY + static_cast<float>(qhiY) * scaleY;
  bounds.maxPoint[2] = parentMinZ + static_cast<float>(qhiZ) * scaleZ;
  return bounds;
}

void expectBoundsEncloses(const virtualgeometry::AABB &decoded, const virtualgeometry::AABB &expected, const std::string &label, float epsilon = kEpsilon)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    expectTrue(
        decoded.minPoint[axis] <= expected.minPoint[axis] + epsilon,
        label + " min[" + std::to_string(axis) + "] decoded=" + std::to_string(decoded.minPoint[axis]) + " expected=" + std::to_string(expected.minPoint[axis]));
    expectTrue(
        decoded.maxPoint[axis] >= expected.maxPoint[axis] - epsilon,
        label + " max[" + std::to_string(axis) + "] decoded=" + std::to_string(decoded.maxPoint[axis]) + " expected=" + std::to_string(expected.maxPoint[axis]));
  }
}

virtualgeometry::AABB tlasCanonicalBounds(const TLASManager::CanonicalNodeGPU &node)
{
  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = node.minPoint[axis];
    bounds.maxPoint[axis] = node.maxPoint[axis];
  }
  return bounds;
}

bool isTlasCanonicalLeaf(const TLASManager::CanonicalNodeGPU &node)
{
  return (node.flags & TLASManager::NODE_FLAG_LEAF) != 0u;
}

void validateTlasCanonicalHierarchy(const std::vector<TLASManager::CanonicalNodeGPU> &canonicalNodes, uint32_t rootNodeIndex, const std::vector<TLASManager::InstanceBuildInput> &instances, const std::string &label)
{
  expectTrue(rootNodeIndex < canonicalNodes.size(), label + " root node index in range");
  std::vector<uint8_t> visited(canonicalNodes.size(), 0u);
  std::vector<uint8_t> seenInstances(instances.size(), 0u);

  const auto validateNode = [&](auto &&self, const uint32_t nodeIndex, const uint32_t expectedParent) -> uint32_t
  {
    expectTrue(nodeIndex < canonicalNodes.size(), label + " canonical node index in range");
    expectTrue(visited[nodeIndex] == 0u, label + " canonical hierarchy should not contain cycles");
    visited[nodeIndex] = 1u;

    const auto &node = canonicalNodes[nodeIndex];
    expectEqual(node.parent, expectedParent, label + " canonical parent[" + std::to_string(nodeIndex) + "]");
    const virtualgeometry::AABB nodeBounds = tlasCanonicalBounds(node);
    expectTrue(nodeBounds.minPoint[0] <= nodeBounds.maxPoint[0], label + " canonical bounds valid x[" + std::to_string(nodeIndex) + "]");
    expectTrue(nodeBounds.minPoint[1] <= nodeBounds.maxPoint[1], label + " canonical bounds valid y[" + std::to_string(nodeIndex) + "]");
    expectTrue(nodeBounds.minPoint[2] <= nodeBounds.maxPoint[2], label + " canonical bounds valid z[" + std::to_string(nodeIndex) + "]");

    if (isTlasCanonicalLeaf(node))
    {
      expectEqual(node.left, TLASManager::INVALID_NODE, label + " leaf left child[" + std::to_string(nodeIndex) + "]");
      expectEqual(node.right, TLASManager::INVALID_NODE, label + " leaf right child[" + std::to_string(nodeIndex) + "]");
      expectEqual(node.subtreeLeafCount, 1u, label + " leaf subtree count[" + std::to_string(nodeIndex) + "]");
      expectTrue(node.instanceIndex < instances.size(), label + " leaf instance index in range[" + std::to_string(nodeIndex) + "]");
      compareBounds(nodeBounds, instances[node.instanceIndex].worldBounds, label + " leaf bounds match instance[" + std::to_string(nodeIndex) + "]");
      seenInstances[node.instanceIndex] = 1u;
      return 1u;
    }

    expectTrue(node.left != TLASManager::INVALID_NODE, label + " internal left child exists[" + std::to_string(nodeIndex) + "]");
    expectTrue(node.right != TLASManager::INVALID_NODE, label + " internal right child exists[" + std::to_string(nodeIndex) + "]");
    expectTrue(node.left < canonicalNodes.size(), label + " internal left child in range[" + std::to_string(nodeIndex) + "]");
    expectTrue(node.right < canonicalNodes.size(), label + " internal right child in range[" + std::to_string(nodeIndex) + "]");
    expectTrue(node.left != node.right, label + " internal children should be distinct[" + std::to_string(nodeIndex) + "]");

    const uint32_t leftLeafCount = self(self, node.left, nodeIndex);
    const uint32_t rightLeafCount = self(self, node.right, nodeIndex);
    const virtualgeometry::AABB leftBounds = tlasCanonicalBounds(canonicalNodes[node.left]);
    const virtualgeometry::AABB rightBounds = tlasCanonicalBounds(canonicalNodes[node.right]);
    expectBoundsEncloses(nodeBounds, leftBounds, label + " parent encloses left child[" + std::to_string(nodeIndex) + "]");
    expectBoundsEncloses(nodeBounds, rightBounds, label + " parent encloses right child[" + std::to_string(nodeIndex) + "]");
    expectEqual(node.subtreeLeafCount, leftLeafCount + rightLeafCount, label + " internal subtree count[" + std::to_string(nodeIndex) + "]");
    return node.subtreeLeafCount;
  };

  expectEqual(canonicalNodes[rootNodeIndex].parent, TLASManager::INVALID_NODE, label + " root parent");
  const uint32_t visitedLeafCount = validateNode(validateNode, rootNodeIndex, TLASManager::INVALID_NODE);
  expectEqual(visitedLeafCount, static_cast<uint32_t>(instances.size()), label + " visited leaf count");

  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(visited.size()); ++nodeIndex)
    expectEqual(visited[nodeIndex], 1u, label + " canonical node reachable[" + std::to_string(nodeIndex) + "]");
  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(seenInstances.size()); ++instanceIndex)
    expectEqual(seenInstances[instanceIndex], 1u, label + " canonical leaf covers instance[" + std::to_string(instanceIndex) + "]");
}

void validateTlasPackedBounds(
    const std::vector<TLASManager::CWBVHNodeGPU> &packedNodes,
    const std::vector<TLASManager::CWBVHNodeRefGPU> &nodeRefs,
    const std::vector<TLASManager::CanonicalNodeGPU> &canonicalNodes,
    const std::string &label)
{
  expectEqual(static_cast<uint32_t>(nodeRefs.size()), static_cast<uint32_t>(packedNodes.size()), label + " node ref count");
  for (uint32_t packedNodeIndex = 0u; packedNodeIndex < static_cast<uint32_t>(packedNodes.size()); ++packedNodeIndex)
  {
    const auto &ref = nodeRefs[packedNodeIndex];
    if (ref.canonicalNodeIndex == TLASManager::INVALID_NODE)
      continue;

    expectTrue(ref.canonicalNodeIndex < canonicalNodes.size(), label + " packed node canonical ref in range");
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      const uint32_t childCanonicalIndex = ref.childCanonicalIndices[childSlot];
      if (childCanonicalIndex == TLASManager::INVALID_NODE)
        continue;
      expectTrue(childCanonicalIndex < canonicalNodes.size(), label + " child canonical ref in range");

      const virtualgeometry::AABB parent = tlasCanonicalBounds(canonicalNodes[ref.canonicalNodeIndex]);
      const virtualgeometry::AABB expected = tlasCanonicalBounds(canonicalNodes[childCanonicalIndex]);
      expectBoundsEncloses(parent, expected, label + " packed parent canonical enclosure[" + std::to_string(packedNodeIndex) + "][" + std::to_string(childSlot) + "]");

      const virtualgeometry::AABB fieldDecoded = decodePackedChildBounds(packedNodes[packedNodeIndex], childSlot);
      const virtualgeometry::AABB shaderDecoded = decodePackedChildBoundsAsShaderVec4s(packedNodes[packedNodeIndex], childSlot);
      compareBounds(shaderDecoded, fieldDecoded, label + " shader-view child bounds[" + std::to_string(packedNodeIndex) + "][" + std::to_string(childSlot) + "]");
      expectBoundsEncloses(shaderDecoded, expected, label + " packed child enclosure[" + std::to_string(packedNodeIndex) + "][" + std::to_string(childSlot) + "]");
    }
  }
}

template <typename CWBVHNode> uint32_t packedChildInfo(const CWBVHNode &node, uint32_t childSlot)
{
  expectTrue(childSlot < 8u, "Packed CWBVH child info slot should be < 8");
  return (node.info[childSlot >> 2u] >> ((childSlot & 3u) * 8u)) & 0xffu;
}

void validateTlasPackedTopology(
    const std::vector<TLASManager::CWBVHNodeGPU> &packedNodes,
    const std::vector<TLASManager::CWBVHNodeRefGPU> &nodeRefs,
    const std::vector<uint32_t> &leafInstanceIndices,
    const std::vector<TLASManager::CanonicalNodeGPU> &canonicalNodes,
    uint32_t expectedInstanceCount,
    const std::string &label)
{
  expectEqual(static_cast<uint32_t>(nodeRefs.size()), static_cast<uint32_t>(packedNodes.size()), label + " packed node ref count");
  std::vector<uint8_t> seenInstances(expectedInstanceCount, 0u);

  for (uint32_t packedNodeIndex = 0u; packedNodeIndex < static_cast<uint32_t>(packedNodes.size()); ++packedNodeIndex)
  {
    const auto &node = packedNodes[packedNodeIndex];
    const auto &ref = nodeRefs[packedNodeIndex];
    if (ref.canonicalNodeIndex == TLASManager::INVALID_NODE)
      continue;

    expectTrue(ref.canonicalNodeIndex < canonicalNodes.size(), label + " packed canonical node ref in range");
    uint32_t internalOrdinal = 0u;
    uint32_t leafCount = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      const uint32_t childCanonicalIndex = ref.childCanonicalIndices[childSlot];
      const uint32_t info = packedChildInfo(node, childSlot);
      if (childCanonicalIndex == TLASManager::INVALID_NODE)
      {
        expectEqual(info, 0u, label + " empty packed child has zero info[" + std::to_string(packedNodeIndex) + "][" + std::to_string(childSlot) + "]");
        continue;
      }

      expectTrue(info != 0u, label + " occupied packed child has metadata[" + std::to_string(packedNodeIndex) + "][" + std::to_string(childSlot) + "]");
      expectTrue(childCanonicalIndex < canonicalNodes.size(), label + " packed child canonical node ref in range");
      const uint32_t localOffset = info & 0x1fu;
      if (isTlasCanonicalLeaf(canonicalNodes[childCanonicalIndex]))
      {
        expectTrue(localOffset < 24u, label + " packed leaf local offset should be < 24");
        const uint32_t leafIndex = node.ntbi[1] + localOffset;
        expectTrue(leafIndex < leafInstanceIndices.size(), label + " packed leaf remap index in range");
        const uint32_t instanceIndex = leafInstanceIndices[leafIndex];
        expectEqual(instanceIndex, canonicalNodes[childCanonicalIndex].instanceIndex, label + " packed leaf remap matches canonical instance");
        expectTrue(instanceIndex < expectedInstanceCount, label + " packed leaf instance index in range");
        seenInstances[instanceIndex] = 1u;
        ++leafCount;
      }
      else
      {
        expectTrue(localOffset >= 24u, label + " packed internal local offset should be >= 24");
        const uint32_t childPackedNodeIndex = node.ntbi[0] + internalOrdinal;
        expectTrue(childPackedNodeIndex < nodeRefs.size(), label + " packed internal child node index in range");
        expectEqual(nodeRefs[childPackedNodeIndex].canonicalNodeIndex, childCanonicalIndex, label + " packed internal child points to canonical child");
        ++internalOrdinal;
      }
    }
    expectTrue(internalOrdinal + leafCount > 0u, label + " packed node should reference at least one child");
  }

  for (uint32_t instanceIndex = 0u; instanceIndex < expectedInstanceCount; ++instanceIndex)
    expectEqual(seenInstances[instanceIndex], 1u, label + " packed leaves cover instance[" + std::to_string(instanceIndex) + "]");
}

void validateTlasPackedLeafCoverageWithoutNodeRefs(
    const std::vector<TLASManager::CWBVHNodeGPU> &packedNodes,
    const std::vector<uint32_t> &leafInstanceIndices,
    const std::vector<TLASManager::InstanceBuildInput> &instances,
    const std::string &label)
{
  expectTrue(!packedNodes.empty(), label + " packed CWBVH should not be empty");
  std::vector<uint8_t> visitedNodes(packedNodes.size(), 0u);
  std::vector<uint8_t> seenInstances(instances.size(), 0u);
  std::vector<uint32_t> stack{0u};

  while (!stack.empty())
  {
    const uint32_t packedNodeIndex = stack.back();
    stack.pop_back();
    expectTrue(packedNodeIndex < packedNodes.size(), label + " packed node index in range");
    expectTrue(visitedNodes[packedNodeIndex] == 0u, label + " packed topology should not contain cycles");
    visitedNodes[packedNodeIndex] = 1u;

    const auto &node = packedNodes[packedNodeIndex];
    uint32_t internalOrdinal = 0u;
    uint32_t childCount = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      const uint32_t info = packedChildInfo(node, childSlot);
      if (info == 0u)
        continue;

      ++childCount;
      const uint32_t localOffset = info & 0x1fu;
      if (localOffset >= 24u)
      {
        const uint32_t childPackedNodeIndex = node.ntbi[0] + internalOrdinal;
        expectTrue(childPackedNodeIndex < packedNodes.size(), label + " internal child packed node index in range");
        stack.push_back(childPackedNodeIndex);
        ++internalOrdinal;
        continue;
      }

      const uint32_t leafIndex = node.ntbi[1] + localOffset;
      expectTrue(leafIndex < leafInstanceIndices.size(), label + " leaf remap index in range");
      const uint32_t instanceIndex = leafInstanceIndices[leafIndex];
      expectTrue(instanceIndex < instances.size(), label + " leaf instance index in range");
      const virtualgeometry::AABB decodedLeaf = decodePackedChildBounds(node, childSlot);
      uint32_t closestInstanceIndex = TLASManager::INVALID_INSTANCE_INDEX;
      float closestDistance = std::numeric_limits<float>::infinity();
      for (uint32_t candidateIndex = 0u; candidateIndex < static_cast<uint32_t>(instances.size()); ++candidateIndex)
      {
        const virtualgeometry::AABB &candidate = instances[candidateIndex].worldBounds;
        float distance = 0.0f;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          const float minDelta = decodedLeaf.minPoint[axis] - candidate.minPoint[axis];
          const float maxDelta = decodedLeaf.maxPoint[axis] - candidate.maxPoint[axis];
          distance += minDelta * minDelta + maxDelta * maxDelta;
        }
        if (distance < closestDistance)
        {
          closestDistance = distance;
          closestInstanceIndex = candidateIndex;
        }
      }
      expectBoundsEncloses(
          decodedLeaf,
          instances[instanceIndex].worldBounds,
          label + " leaf bounds enclose instance[" + std::to_string(instanceIndex) + "] packedNode=" + std::to_string(packedNodeIndex) + " childSlot=" + std::to_string(childSlot) +
              " leafIndex=" + std::to_string(leafIndex) + " localOffset=" + std::to_string(localOffset) + " nodeLeafBase=" + std::to_string(node.ntbi[1]) + " closestInstance=" + std::to_string(closestInstanceIndex));
      seenInstances[instanceIndex] = 1u;
    }
    expectTrue(childCount > 0u, label + " packed node should contain children");
  }

  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(instances.size()); ++instanceIndex)
    expectEqual(seenInstances[instanceIndex], 1u, label + " packed leaves cover instance[" + std::to_string(instanceIndex) + "]");
}

math::Vec3f aabbCenter(const virtualgeometry::AABB &bounds)
{
  return math::Vec3f((bounds.minPoint[0] + bounds.maxPoint[0]) * 0.5f, (bounds.minPoint[1] + bounds.maxPoint[1]) * 0.5f, (bounds.minPoint[2] + bounds.maxPoint[2]) * 0.5f);
}

float aabbDiagonalLength(const virtualgeometry::AABB &bounds)
{
  const float dx = bounds.maxPoint[0] - bounds.minPoint[0];
  const float dy = bounds.maxPoint[1] - bounds.minPoint[1];
  const float dz = bounds.maxPoint[2] - bounds.minPoint[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool rayIntersectsAabb(const math::Vec3f &origin, const math::Vec3f &direction, const virtualgeometry::AABB &bounds)
{
  float tmin = 0.0f;
  float tmax = std::numeric_limits<float>::infinity();
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float dir = direction[axis];
    const float minPoint = bounds.minPoint[axis];
    const float maxPoint = bounds.maxPoint[axis];
    if (std::fabs(dir) <= 1.0e-12f)
    {
      if (origin[axis] < minPoint || origin[axis] > maxPoint)
        return false;
      continue;
    }

    float nearT = (minPoint - origin[axis]) / dir;
    float farT = (maxPoint - origin[axis]) / dir;
    if (nearT > farT)
      std::swap(nearT, farT);
    tmin = std::max(tmin, nearT);
    tmax = std::min(tmax, farT);
    if (tmin > tmax)
      return false;
  }
  return true;
}

void validateTlasPackedRayReachabilityWithoutNodeRefs(
    const std::vector<TLASManager::CWBVHNodeGPU> &packedNodes,
    const std::vector<uint32_t> &leafInstanceIndices,
    const std::vector<TLASManager::InstanceBuildInput> &instances,
    const std::string &label)
{
  expectTrue(!packedNodes.empty(), label + " packed CWBVH should not be empty");
  virtualgeometry::AABB sceneBounds = instances.front().worldBounds;
  for (const TLASManager::InstanceBuildInput &instance : instances)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      sceneBounds.minPoint[axis] = std::min(sceneBounds.minPoint[axis], instance.worldBounds.minPoint[axis]);
      sceneBounds.maxPoint[axis] = std::max(sceneBounds.maxPoint[axis], instance.worldBounds.maxPoint[axis]);
    }
  }

  const float sceneRayDistance = std::max(1.0f, aabbDiagonalLength(sceneBounds) * 2.0f);
  for (uint32_t targetInstance = 0u; targetInstance < static_cast<uint32_t>(instances.size()); ++targetInstance)
  {
    const math::Vec3f target = aabbCenter(instances[targetInstance].worldBounds);
    const math::Vec3f origin = target + math::Vec3f(0.0f, 0.0f, sceneRayDistance);
    const math::Vec3f direction = (target - origin).normalize();
    expectTrue(rayIntersectsAabb(origin, direction, instances[targetInstance].worldBounds), label + " test ray should intersect target instance bounds[" + std::to_string(targetInstance) + "]");

    bool reachedTarget = false;
    std::vector<uint32_t> stack{0u};
    std::vector<uint32_t> visitCount(packedNodes.size(), 0u);
    while (!stack.empty())
    {
      const uint32_t packedNodeIndex = stack.back();
      stack.pop_back();
      expectTrue(packedNodeIndex < packedNodes.size(), label + " ray traversal packed node index in range");
      expectTrue(++visitCount[packedNodeIndex] <= 1u, label + " ray traversal should not revisit packed node");

      const TLASManager::CWBVHNodeGPU &node = packedNodes[packedNodeIndex];
      uint32_t internalOrdinal = 0u;
      for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
      {
        const uint32_t info = packedChildInfo(node, childSlot);
        if (info == 0u)
          continue;

        const virtualgeometry::AABB childBounds = decodePackedChildBounds(node, childSlot);
        const bool intersects = rayIntersectsAabb(origin, direction, childBounds);
        const uint32_t localOffset = info & 0x1fu;
        if (localOffset >= 24u)
        {
          const uint32_t childPackedNodeIndex = node.ntbi[0] + internalOrdinal;
          expectTrue(childPackedNodeIndex < packedNodes.size(), label + " ray traversal internal child index in range");
          if (intersects)
            stack.push_back(childPackedNodeIndex);
          ++internalOrdinal;
          continue;
        }

        if (!intersects)
          continue;
        const uint32_t leafIndex = node.ntbi[1] + localOffset;
        expectTrue(leafIndex < leafInstanceIndices.size(), label + " ray traversal leaf index in range");
        if (leafInstanceIndices[leafIndex] == targetInstance)
          reachedTarget = true;
      }
    }

    expectTrue(reachedTarget, label + " center ray reaches target instance leaf[" + std::to_string(targetInstance) + "]");
  }
}

void validateTlasLeafInstanceRemap(const std::vector<uint32_t> &leafInstanceIndices, uint32_t expectedInstanceCount, const std::string &label)
{
  expectEqual(static_cast<uint32_t>(leafInstanceIndices.size()), expectedInstanceCount, label + " leaf remap count");
  std::vector<uint8_t> seen(expectedInstanceCount, 0u);
  for (uint32_t leafIndex = 0u; leafIndex < static_cast<uint32_t>(leafInstanceIndices.size()); ++leafIndex)
  {
    const uint32_t instanceIndex = leafInstanceIndices[leafIndex];
    expectTrue(instanceIndex < expectedInstanceCount, label + " leaf remap instance in range[" + std::to_string(leafIndex) + "]");
    expectEqual(seen[instanceIndex], 0u, label + " leaf remap instance is unique[" + std::to_string(instanceIndex) + "]");
    seen[instanceIndex] = 1u;
  }
  for (uint32_t instanceIndex = 0u; instanceIndex < expectedInstanceCount; ++instanceIndex)
    expectEqual(seen[instanceIndex], 1u, label + " leaf remap covers instance[" + std::to_string(instanceIndex) + "]");
}

struct GenericCwbvhReadback
{
  uint32_t nodeCount = 0u;
  uint32_t leafCount = 0u;
  std::vector<TLASManager::CWBVHNodeGPU> nodes;
  std::vector<uint32_t> leafInstanceIndices;
};

GenericCwbvhReadback readGenericTlasGpuCwbvhReadback(rendering::RenderGraph &renderGraph, const TLASManager &manager, uint32_t expectedLeafCount, const std::string &label)
{
  const auto signals = readBufferVector<uint32_t>(renderGraph, manager.getBuildSignalsBuffer(), 6u);
  expectTrue(signals.size() >= 5u, label + " build signals readback count");

  GenericCwbvhReadback readback{};
  readback.nodeCount = signals[3u];
  readback.leafCount = signals[4u];
  expectTrue(readback.nodeCount > 0u, label + " GPU CWBVH node count should be non-zero");
  expectTrue(readback.nodeCount <= manager.getMaxInstanceCount(), label + " GPU CWBVH node count should fit TLAS capacity");
  expectEqual(readback.leafCount, expectedLeafCount, label + " GPU CWBVH leaf remap count");
  expectEqual(signals[5u], readback.nodeCount, label + " GPU CWBVH completed node count");

  readback.nodes = readBufferVector<TLASManager::CWBVHNodeGPU>(renderGraph, manager.getCwbvhNodesBuffer(), readback.nodeCount);
  readback.leafInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getLeafInstanceIndicesBuffer(), readback.leafCount);
  return readback;
}

void validateGenericTlasGpuHlbvhLeaves(rendering::RenderGraph &renderGraph, const TLASManager &manager, const std::vector<TLASManager::InstanceBuildInput> &instances, const std::string &label)
{
  const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
  const auto sortedInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getBuildInstanceIndicesBuffer(), instanceCount);
  const auto hlbvhNodes = readBufferVector<TLASManager::GPUBuildHLBVHNodeGPU>(renderGraph, manager.getBuildHLBVHNodesBuffer(), instanceCount * 2u - 1u);

  for (uint32_t sortedIndex = 0u; sortedIndex < instanceCount; ++sortedIndex)
  {
    const uint32_t instanceIndex = sortedInstanceIndices[sortedIndex];
    expectTrue(instanceIndex < instanceCount, label + " sorted instance index in range[" + std::to_string(sortedIndex) + "]");
    const uint32_t leafNodeIndex = instanceCount - 1u + sortedIndex;
    const auto &leaf = hlbvhNodes[leafNodeIndex];
    expectEqual(leaf.left, instanceIndex, label + " HLBVH leaf left remap[" + std::to_string(sortedIndex) + "]");
    expectEqual(leaf.right, instanceIndex, label + " HLBVH leaf right remap[" + std::to_string(sortedIndex) + "]");

    virtualgeometry::AABB leafBounds{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      leafBounds.minPoint[axis] = leaf.minPoint[axis];
      leafBounds.maxPoint[axis] = leaf.maxPoint[axis];
    }
    compareBounds(leafBounds, instances[instanceIndex].worldBounds, label + " HLBVH leaf bounds[" + std::to_string(sortedIndex) + "]");
  }
}

void validateGenericTlasGpuHlbvhTopology(rendering::RenderGraph &renderGraph, const TLASManager &manager, const std::vector<TLASManager::InstanceBuildInput> &instances, const std::string &label)
{
  const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
  const uint32_t nodeCount = instanceCount * 2u - 1u;
  const uint32_t firstLeafNode = instanceCount - 1u;
  const auto hlbvhNodes = readBufferVector<TLASManager::GPUBuildHLBVHNodeGPU>(renderGraph, manager.getBuildHLBVHNodesBuffer(), nodeCount);
  std::vector<uint8_t> visited(nodeCount, 0u);
  std::vector<uint32_t> stack{0u};

  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    expectTrue(nodeIndex < nodeCount, label + " HLBVH node index in range");
    expectEqual(visited[nodeIndex], 0u, label + " HLBVH topology should not contain cycles");
    visited[nodeIndex] = 1u;

    if (nodeIndex >= firstLeafNode)
      continue;

    const auto &node = hlbvhNodes[nodeIndex];
    expectTrue(node.left < nodeCount, label + " HLBVH left child in range[" + std::to_string(nodeIndex) + "]");
    expectTrue(node.right < nodeCount, label + " HLBVH right child in range[" + std::to_string(nodeIndex) + "]");
    expectTrue(node.left != node.right, label + " HLBVH internal children distinct[" + std::to_string(nodeIndex) + "]");
    expectEqual(hlbvhNodes[node.left].parent, nodeIndex, label + " HLBVH left parent[" + std::to_string(nodeIndex) + "]");
    expectEqual(hlbvhNodes[node.right].parent, nodeIndex, label + " HLBVH right parent[" + std::to_string(nodeIndex) + "]");

    virtualgeometry::AABB parentBounds{};
    virtualgeometry::AABB leftBounds{};
    virtualgeometry::AABB rightBounds{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      parentBounds.minPoint[axis] = node.minPoint[axis];
      parentBounds.maxPoint[axis] = node.maxPoint[axis];
      leftBounds.minPoint[axis] = hlbvhNodes[node.left].minPoint[axis];
      leftBounds.maxPoint[axis] = hlbvhNodes[node.left].maxPoint[axis];
      rightBounds.minPoint[axis] = hlbvhNodes[node.right].minPoint[axis];
      rightBounds.maxPoint[axis] = hlbvhNodes[node.right].maxPoint[axis];
    }
    expectBoundsEncloses(parentBounds, leftBounds, label + " HLBVH parent encloses left[" + std::to_string(nodeIndex) + "]");
    expectBoundsEncloses(parentBounds, rightBounds, label + " HLBVH parent encloses right[" + std::to_string(nodeIndex) + "]");

    stack.push_back(node.left);
    stack.push_back(node.right);
  }

  for (uint32_t nodeIndex = 0u; nodeIndex < nodeCount; ++nodeIndex)
    expectEqual(visited[nodeIndex], 1u, label + " HLBVH node reachable[" + std::to_string(nodeIndex) + "]");
}

template <typename CWBVHNode> std::vector<std::pair<uint32_t, uint32_t>> collectPackedLeafSlots(const CWBVHNode &node, const std::vector<uint32_t> &leafRemap)
{
  std::vector<std::pair<uint32_t, uint32_t>> leaves;
  for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
  {
    const uint32_t info = packedChildInfo(node, childSlot);
    if (info == 0u)
    {
      continue;
    }

    const uint32_t localOffset = info & 0x1fu;
    if (localOffset >= 24u)
    {
      continue;
    }

    expectTrue(localOffset < leafRemap.size(), "Packed CWBVH leaf offset should address leaf remap");
    leaves.emplace_back(childSlot, leafRemap[localOffset]);
  }
  return leaves;
}

template <typename CWBVHNode> uint32_t packedLeafSlotCount(const CWBVHNode &node)
{
  uint32_t count = 0u;
  for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
  {
    const uint32_t info = packedChildInfo(node, childSlot);
    if (info == 0u)
      continue;

    if ((info & 0x1fu) < 24u)
      ++count;
  }
  return count;
}

template <typename CWBVHNode> bool packedNodeHasLeafBeforeInternalChild(const CWBVHNode &node)
{
  bool sawLeaf = false;
  for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
  {
    const uint32_t info = packedChildInfo(node, childSlot);
    if (info == 0u)
      continue;

    const uint32_t localOffset = info & 0x1fu;
    if (localOffset >= 24u)
    {
      if (sawLeaf)
        return true;
      continue;
    }

    sawLeaf = true;
  }
  return false;
}

template <typename CWBVHNode> void validatePackedInternalChildLinksWithoutNodeRefs(const std::vector<CWBVHNode> &packedNodes, const std::string &label)
{
  expectTrue(!packedNodes.empty(), label + " packed CWBVH should not be empty");
  std::vector<uint8_t> visited(packedNodes.size(), 0u);
  std::vector<uint32_t> stack{0u};

  while (!stack.empty())
  {
    const uint32_t packedNodeIndex = stack.back();
    stack.pop_back();
    expectTrue(packedNodeIndex < packedNodes.size(), label + " packed node index in range");
    expectEqual(visited[packedNodeIndex], 0u, label + " packed child links should not contain cycles");
    visited[packedNodeIndex] = 1u;

    const CWBVHNode &node = packedNodes[packedNodeIndex];
    uint32_t internalOrdinal = 0u;
    uint32_t occupiedChildCount = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      const uint32_t info = packedChildInfo(node, childSlot);
      if (info == 0u)
        continue;

      ++occupiedChildCount;
      const uint32_t localOffset = info & 0x1fu;
      if (localOffset < 24u)
        continue;

      const uint32_t childPackedNodeIndex = node.ntbi[0] + internalOrdinal;
      expectTrue(childPackedNodeIndex < packedNodes.size(), label + " internal child packed node index in range");
      expectTrue(childPackedNodeIndex != packedNodeIndex, label + " internal child should not point to itself");
      stack.push_back(childPackedNodeIndex);
      ++internalOrdinal;
    }
    expectTrue(occupiedChildCount > 0u, label + " packed node should contain children");
  }

  for (uint32_t packedNodeIndex = 0u; packedNodeIndex < static_cast<uint32_t>(packedNodes.size()); ++packedNodeIndex)
    expectEqual(visited[packedNodeIndex], 1u, label + " packed node reachable from root[" + std::to_string(packedNodeIndex) + "]");
}

template <typename CWBVHNode>
void validateBlasPackedLeafCoverageWithoutNodeRefs(const std::vector<CWBVHNode> &packedNodes, const std::vector<uint32_t> &leafRemap, const virtualgeometry::FallbackMeshData &mesh, const std::string &label)
{
  expectTrue(!packedNodes.empty(), label + " packed CWBVH should not be empty");
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  std::vector<uint8_t> visitedNodes(packedNodes.size(), 0u);
  std::vector<uint8_t> seenPrimitives(primitiveCount, 0u);
  std::vector<uint32_t> stack{0u};

  while (!stack.empty())
  {
    const uint32_t packedNodeIndex = stack.back();
    stack.pop_back();
    expectTrue(packedNodeIndex < packedNodes.size(), label + " packed node index in range");
    expectEqual(visitedNodes[packedNodeIndex], 0u, label + " packed topology should not contain cycles");
    visitedNodes[packedNodeIndex] = 1u;

    const CWBVHNode &node = packedNodes[packedNodeIndex];
    uint32_t internalOrdinal = 0u;
    uint32_t childCount = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      const uint32_t info = packedChildInfo(node, childSlot);
      if (info == 0u)
        continue;

      ++childCount;
      const uint32_t localOffset = info & 0x1fu;
      if (localOffset >= 24u)
      {
        const uint32_t childPackedNodeIndex = node.ntbi[0] + internalOrdinal;
        expectTrue(childPackedNodeIndex < packedNodes.size(), label + " internal child packed node index in range");
        stack.push_back(childPackedNodeIndex);
        ++internalOrdinal;
        continue;
      }

      const uint32_t leafIndex = node.ntbi[1] + localOffset;
      expectTrue(leafIndex < leafRemap.size(), label + " leaf remap index in range");
      const uint32_t primitiveIndex = leafRemap[leafIndex];
      expectTrue(primitiveIndex < primitiveCount, label + " leaf primitive index in range");
      const virtualgeometry::AABB decodedLeaf = decodePackedChildBounds(node, childSlot);
      expectBoundsEncloses(decodedLeaf, triangleBounds(mesh, primitiveIndex), label + " leaf bounds enclose primitive[" + std::to_string(primitiveIndex) + "]");
      seenPrimitives[primitiveIndex] = 1u;
    }
    expectTrue(childCount > 0u, label + " packed node should contain children");
  }

  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(visitedNodes.size()); ++nodeIndex)
    expectEqual(visitedNodes[nodeIndex], 1u, label + " packed node reachable from root[" + std::to_string(nodeIndex) + "]");
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
    expectEqual(seenPrimitives[primitiveIndex], 1u, label + " packed leaves cover primitive[" + std::to_string(primitiveIndex) + "]");
}

BLASManager::Settings blasTestSettings()
{
  BLASManager::Settings settings{};
  settings.maxBlases = 8u;
  settings.maxVertices = 128u;
  settings.maxIndices = 256u;
  settings.maxNodes = 128u;
  settings.maxPrimitives = 64u;
  settings.maxPackedCWBVHNodes = 128u;
  settings.maxRbvhDepthMipTexels = 64u;
  settings.frameLocalBuffers = false;
  settings.workgroupSize = 64u;
  return settings;
}

BLASManager::Settings blasTestSettingsForMesh(const virtualgeometry::FallbackMeshData &mesh, const softwarebvh::CWBVHBuilder::BuildResult *prebuilt = nullptr, const softwarebvh::RBVHBuildResult *rbvhBuild = nullptr)
{
  const softwarebvh::CWBVHBuilder::BuildResult ownedBuild = (prebuilt == nullptr && rbvhBuild == nullptr) ? softwarebvh::CWBVHBuilder::build(mesh) : softwarebvh::CWBVHBuilder::BuildResult{};
  const softwarebvh::CWBVHBuilder::BuildResult &build = rbvhBuild != nullptr ? rbvhBuild->cardBuild : (prebuilt != nullptr ? *prebuilt : ownedBuild);
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  const uint32_t buildPrimitiveCount = rbvhBuild != nullptr ? static_cast<uint32_t>(rbvhBuild->cards.size()) : primitiveCount;

  BLASManager::Settings settings{};
  settings.maxBlases = 8u;
  settings.maxVertices = std::max<uint32_t>(static_cast<uint32_t>(mesh.vertices.size()) + 16u, 128u);
  settings.maxIndices = std::max<uint32_t>(static_cast<uint32_t>(mesh.indices.size()) + 32u, 256u);
  settings.maxNodes = std::max<uint32_t>(static_cast<uint32_t>(build.canonicalNodes.size()) + 16u, 128u);
  settings.maxPrimitives = std::max<uint32_t>(buildPrimitiveCount + 16u, 64u);
  settings.maxPackedCWBVHNodes = std::max<uint32_t>(static_cast<uint32_t>(build.canonicalNodes.size()) + 16u, 128u);
  settings.maxRbvhCards = std::max<uint32_t>(buildPrimitiveCount + 16u, 64u);
  settings.maxRbvhAtlasTexels = std::max<uint32_t>(rbvhBuild != nullptr ? static_cast<uint32_t>(rbvhBuild->depthAtlasR16.size()) + 16u : 64u, 64u);
  settings.maxRbvhDepthMipTexels = std::max<uint32_t>(rbvhBuild != nullptr ? static_cast<uint32_t>(rbvhBuild->depthMinMaxMipAtlasRG16.size()) + 16u : 64u, 64u);
  settings.frameLocalBuffers = false;
  settings.workgroupSize = 64u;
  return settings;
}

TLASManager::Settings tlasTestSettings()
{
  TLASManager::Settings settings{};
  settings.maxInstances = 16u;
  settings.frameLocalBuffers = false;
  settings.workgroupSize = 64u;
  return settings;
}

std::vector<rendering::Pass *> registerTlasGpuRebuildPasses(rendering::RenderGraph &renderGraph, TLASManager &manager, uint32_t basePassIndex, const std::string &prefix)
{
  using TLASPass = softwarebvh::rendering::SoftwareTLASPass;
  std::vector<rendering::Pass *> passes;
  passes.reserve(8u);
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".prepare", basePassIndex + 0u, manager, TLASPass::Stage::PrepareRebuild));
  auto *sortPass = renderGraph.registerPass<rendering::gpgpu::RadixSort2Pass<uint32_t, uint32_t>>(
      prefix + ".sort",
      basePassIndex + 1u,
      manager.getMaxInstanceCount(),
      manager.getBuildMortonCodesBuffer(),
      manager.getBuildInstanceIndicesBuffer(),
      manager.getBuildSortCountBuffer(),
      manager.getBuildSortIndirectArgsBuffer());
  passes.push_back(sortPass);
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".clearFlags", basePassIndex + 2u, manager, TLASPass::Stage::ClearBuildFlags));
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".buildLeaves", basePassIndex + 3u, manager, TLASPass::Stage::BuildHierarchyLeaves));
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".buildHierarchy", basePassIndex + 4u, manager, TLASPass::Stage::BuildHierarchy));
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".refitHierarchy", basePassIndex + 5u, manager, TLASPass::Stage::RefitHierarchy));
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".initCwbvh", basePassIndex + 6u, manager, TLASPass::Stage::InitializeCWBVH));
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".buildCwbvh", basePassIndex + 7u, manager, TLASPass::Stage::BuildCWBVH));
  return passes;
}

std::vector<rendering::Pass *> registerTlasGpuRefitPasses(rendering::RenderGraph &renderGraph, TLASManager &manager, uint32_t basePassIndex, const std::string &prefix)
{
  using TLASPass = softwarebvh::rendering::SoftwareTLASPass;
  std::vector<rendering::Pass *> passes;
  passes.reserve(2u);
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".refitCanonical", basePassIndex + 0u, manager, TLASPass::Stage::RefitCanonical));
  passes.push_back(renderGraph.registerPass<TLASPass>(prefix + ".refitCwbvh", basePassIndex + 1u, manager, TLASPass::Stage::RefitCWBVH));
  return passes;
}

void deletePasses(std::vector<rendering::Pass *> &passes)
{
  for (auto it = passes.rbegin(); it != passes.rend(); ++it)
  {
    delete *it;
  }
  passes.clear();
}

void testBlasCpuBuildAndBuffers(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager manager(&renderGraph, blasTestSettings());
  const virtualgeometry::FallbackMeshData mesh = makeSeparatedTrianglesMesh();
  const auto expected = softwarebvh::CWBVHBuilder::build(mesh);

  BLASManager::BuildInput input{};
  input.sourcePath = "tests/separated_triangles.mesh";
  input.fallbackPath = "tests/separated_triangles.fallback.mesh";
  input.mesh = mesh;
  const BLASManager::BuildSummary summary = manager.syncFromCPU({input});

  expectEqual(summary.blasCount, 1u, "BLAS CPU build should produce one BLAS");
  expectEqual(summary.primitiveCount, expected.primitiveCount, "BLAS CPU build primitive count");
  expectEqual(summary.nodeCount, static_cast<uint32_t>(expected.canonicalNodes.size()), "BLAS CPU build node count");
  expectTrue(summary.rebuilt, "BLAS CPU build should mark rebuilt");
  expectEqual(summary.buildRevision, 1u, "BLAS CPU build revision");

  const uint32_t handle = manager.findHandleBySourcePath(input.sourcePath);
  expectTrue(handle != BLASManager::INVALID_BLAS_HANDLE, "BLAS handle lookup should succeed");

  virtualgeometry::AABB localBounds{};
  expectTrue(manager.tryGetLocalBounds(handle, &localBounds), "BLAS local bounds lookup should succeed");
  compareBounds(localBounds, expected.localBounds, "BLAS local bounds");

  BLASManager::GPUBuildDescriptor descriptor{};
  expectTrue(manager.tryGetGPUBuildDescriptor(handle, &descriptor), "BLAS descriptor lookup should succeed");
  expectEqual(descriptor.handle, handle, "BLAS descriptor handle");
  expectEqual(descriptor.vertexCount, static_cast<uint32_t>(mesh.vertices.size()), "BLAS descriptor vertex count");
  expectEqual(descriptor.indexCount, static_cast<uint32_t>(mesh.indices.size()), "BLAS descriptor index count");
  expectEqual(descriptor.primitiveCount, expected.primitiveCount, "BLAS descriptor primitive count");
  compareBounds(descriptor.localBounds, expected.localBounds, "BLAS descriptor bounds");

  const auto metadata = readBufferValue<BLASManager::TriangleBLASMetadataGPU>(renderGraph, manager.getTriangleMetadataBuffer(), static_cast<uint64_t>(handle) * sizeof(BLASManager::TriangleBLASMetadataGPU));
  expectEqual(metadata.handleId, handle, "Triangle BLAS metadata handle");
  expectEqual(metadata.primitiveCount, descriptor.primitiveCount, "Triangle BLAS metadata primitive count");
  expectEqual(metadata.cwbvhNodeOffset, descriptor.gpuCWBVHNodeOffset, "Triangle BLAS metadata CWBVH node offset");
  expectEqual(metadata.cwbvhNodeCount, descriptor.gpuCWBVHNodeCount, "Triangle BLAS metadata CWBVH node count");
  expectEqual(metadata.leafRemapOffset, descriptor.gpuLeafRemapOffset, "Triangle BLAS metadata leaf remap offset");
  expectEqual(metadata.vertexOffset, descriptor.vertexOffset, "Triangle BLAS metadata vertex offset");
  expectEqual(metadata.indexOffset, descriptor.indexOffset, "Triangle BLAS metadata index offset");

  const auto canonicalNodes = readBufferVector<BLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), static_cast<uint32_t>(expected.canonicalNodes.size()));
  expectEqual(static_cast<uint32_t>(canonicalNodes.size()), static_cast<uint32_t>(expected.canonicalNodes.size()), "BLAS canonical node vector size");
  for (uint32_t nodeIndex = 0u; nodeIndex < canonicalNodes.size(); ++nodeIndex)
  {
    const auto &actual = canonicalNodes[nodeIndex];
    const auto &expectedNode = expected.canonicalNodes[nodeIndex];
    expectEqual(actual.parent, expectedNode.parent, "BLAS canonical parent[" + std::to_string(nodeIndex) + "]");
    expectEqual(actual.left, expectedNode.left, "BLAS canonical left[" + std::to_string(nodeIndex) + "]");
    expectEqual(actual.right, expectedNode.right, "BLAS canonical right[" + std::to_string(nodeIndex) + "]");
    expectEqual(
        actual.primitiveIndex,
        expectedNode.primitiveIndex == softwarebvh::CWBVHBuilder::INVALID_PRIMITIVE_INDEX ? BLASManager::INVALID_PRIMITIVE_INDEX : expectedNode.primitiveIndex,
        "BLAS canonical primitive[" + std::to_string(nodeIndex) + "]");
    expectEqual(actual.subtreePrimitiveCount, expectedNode.subtreePrimitiveCount, "BLAS canonical subtree count[" + std::to_string(nodeIndex) + "]");
    expectEqual((actual.flags & BLASManager::NODE_FLAG_LEAF) != 0u ? 1u : 0u, expectedNode.isLeaf ? 1u : 0u, "BLAS canonical leaf flag[" + std::to_string(nodeIndex) + "]");

    virtualgeometry::AABB actualBounds{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      actualBounds.minPoint[axis] = actual.minPoint[axis];
      actualBounds.maxPoint[axis] = actual.maxPoint[axis];
    }
    compareBounds(actualBounds, expectedNode.bounds, "BLAS canonical bounds[" + std::to_string(nodeIndex) + "]");
  }

  const auto counters = readBufferValue<BLASManager::CountersGPU>(renderGraph, manager.getCountersBuffer());
  expectEqual(counters.blasCount, 1u, "BLAS counters BLAS count");
  expectEqual(counters.primitiveCount, expected.primitiveCount, "BLAS counters primitive count");
  expectEqual(counters.nodeCount, static_cast<uint32_t>(expected.canonicalNodes.size()), "BLAS counters node count");
  expectEqual(counters.buildRevision, 1u, "BLAS counters build revision");
  expectEqual(counters.buildRequested, 1u, "BLAS counters build requested");
}

void testSwrtFallbackCollectionBuildsDistinctBlases(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager manager(&renderGraph, blasTestSettings());

  std::vector<virtualgeometry::FallbackMeshData> sourceMeshes;
  sourceMeshes.push_back(translateMesh(makeFrontFacingTriangleMesh(0.0f), math::Vec3f(-4.0f, 0.0f, 0.0f)));
  sourceMeshes.push_back(translateMesh(makeFrontFacingTriangleMesh(0.0f), math::Vec3f(1.5f, 0.0f, 0.0f)));
  sourceMeshes.push_back(translateMesh(makeFrontFacingTriangleMesh(0.0f), math::Vec3f(7.0f, 0.0f, 0.0f)));

  std::vector<softwarebvh::SWRTFallbackMeshData> swrtEntries;
  swrtEntries.reserve(sourceMeshes.size());
  for (const virtualgeometry::FallbackMeshData &mesh : sourceMeshes)
    swrtEntries.push_back(softwarebvh::SWRTFallbackMeshFile::build(mesh));

  const fs::path tempPath = fs::temp_directory_path() / "SoftwareBVHRuntimeTests_multi_shape.SWRTFallback.mesh";
  expectTrue(softwarebvh::SWRTFallbackMeshFile::save(tempPath.string(), swrtEntries), "Saving multi-shape SWRT fallback collection should succeed");

  std::vector<softwarebvh::SWRTFallbackMeshData> loadedEntries;
  expectTrue(softwarebvh::SWRTFallbackMeshFile::load(tempPath.string(), loadedEntries), "Loading multi-shape SWRT fallback collection should succeed");
  std::error_code error;
  fs::remove(tempPath, error);

  expectEqual(static_cast<uint32_t>(loadedEntries.size()), static_cast<uint32_t>(sourceMeshes.size()), "Loaded SWRT fallback collection entry count");

  std::vector<BLASManager::BuildInput> inputs;
  inputs.reserve(loadedEntries.size());
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(loadedEntries.size()); ++shapeIndex)
  {
    compareBounds(meshBounds(loadedEntries[shapeIndex].fallbackMesh), meshBounds(sourceMeshes[shapeIndex]), "Loaded SWRT fallback mesh bounds[" + std::to_string(shapeIndex) + "]");
    compareBounds(loadedEntries[shapeIndex].build.localBounds, meshBounds(sourceMeshes[shapeIndex]), "Loaded SWRT fallback prebuilt bounds[" + std::to_string(shapeIndex) + "]");

    BLASManager::BuildInput input{};
    input.sourcePath = "tests/multi_shape.vg::shape" + std::to_string(shapeIndex);
    input.fallbackPath = tempPath.string();
    input.mesh = loadedEntries[shapeIndex].fallbackMesh;
    input.prebuilt = loadedEntries[shapeIndex].build;
    inputs.push_back(std::move(input));
  }

  const BLASManager::BuildSummary summary = manager.syncFromCPU(inputs);
  expectEqual(summary.blasCount, static_cast<uint32_t>(inputs.size()), "Multi-shape SWRT fallback should build one BLAS per shape");

  uint32_t previousCwbvhNodeOffset = BLASManager::INVALID_NODE;
  uint32_t previousLeafRemapOffset = BLASManager::INVALID_NODE;
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(inputs.size()); ++shapeIndex)
  {
    const uint32_t handle = manager.findHandleBySourcePath(inputs[shapeIndex].sourcePath);
    expectTrue(handle != BLASManager::INVALID_BLAS_HANDLE, "Multi-shape BLAS handle lookup should succeed[" + std::to_string(shapeIndex) + "]");

    BLASManager::GPUBuildDescriptor descriptor{};
    expectTrue(manager.tryGetGPUBuildDescriptor(handle, &descriptor), "Multi-shape BLAS descriptor lookup should succeed[" + std::to_string(shapeIndex) + "]");
    compareBounds(descriptor.localBounds, meshBounds(sourceMeshes[shapeIndex]), "Multi-shape BLAS descriptor bounds[" + std::to_string(shapeIndex) + "]");
    expectEqual(descriptor.primitiveCount, loadedEntries[shapeIndex].build.primitiveCount, "Multi-shape BLAS primitive count[" + std::to_string(shapeIndex) + "]");
    expectTrue(descriptor.gpuCWBVHNodeCount > 0u, "Multi-shape BLAS should have packed CWBVH nodes[" + std::to_string(shapeIndex) + "]");

    if (shapeIndex > 0u)
    {
      expectTrue(descriptor.gpuCWBVHNodeOffset > previousCwbvhNodeOffset, "Multi-shape BLAS CWBVH offsets should be distinct and increasing");
      expectTrue(descriptor.gpuLeafRemapOffset > previousLeafRemapOffset, "Multi-shape BLAS leaf remap offsets should be distinct and increasing");
    }
    previousCwbvhNodeOffset = descriptor.gpuCWBVHNodeOffset;
    previousLeafRemapOffset = descriptor.gpuLeafRemapOffset;

    const auto metadata = readBufferValue<BLASManager::TriangleBLASMetadataGPU>(renderGraph, manager.getTriangleMetadataBuffer(), static_cast<uint64_t>(handle) * sizeof(BLASManager::TriangleBLASMetadataGPU));
    expectEqual(metadata.handleId, handle, "Multi-shape triangle BLAS metadata handle[" + std::to_string(shapeIndex) + "]");
    expectEqual(metadata.cwbvhNodeOffset, descriptor.gpuCWBVHNodeOffset, "Multi-shape triangle BLAS metadata CWBVH offset[" + std::to_string(shapeIndex) + "]");
    expectEqual(metadata.leafRemapOffset, descriptor.gpuLeafRemapOffset, "Multi-shape triangle BLAS metadata leaf remap offset[" + std::to_string(shapeIndex) + "]");
    expectEqual(metadata.vertexOffset, descriptor.vertexOffset, "Multi-shape triangle BLAS metadata vertex offset[" + std::to_string(shapeIndex) + "]");
    expectEqual(metadata.indexOffset, descriptor.indexOffset, "Multi-shape triangle BLAS metadata index offset[" + std::to_string(shapeIndex) + "]");
  }
}

void testSwrtFallbackRtFilePreservesAllInstancesAndTriangles()
{
  const virtualgeometry::VirtualGeometrySourceAsset sourceAsset = makeMultiShapeSourceAsset();
  const std::vector<virtualgeometry::FallbackMeshData> expectedFallbackMeshes = virtualgeometry::FallbackMeshBuilder::buildOriginalPerShape(sourceAsset);
  expectEqual(static_cast<uint32_t>(expectedFallbackMeshes.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Generated fallback mesh instance count");

  uint32_t expectedTriangleCount = 0u;
  std::vector<softwarebvh::SWRTFallbackMeshData> expectedRtEntries;
  expectedRtEntries.reserve(expectedFallbackMeshes.size());
  for (const virtualgeometry::FallbackMeshData &mesh : expectedFallbackMeshes)
  {
    expectTrue(mesh.indices.size() % 3u == 0u, "Generated fallback mesh should contain whole triangles");
    expectedTriangleCount += static_cast<uint32_t>(mesh.indices.size() / 3u);
    expectedRtEntries.push_back(softwarebvh::SWRTFallbackMeshFile::build(mesh));
  }

  const fs::path fallbackPath = makeTempPath("SoftwareBVHRuntimeTests_multi_shape.fallback.mesh");
  const fs::path rtPath = makeTempPath("SoftwareBVHRuntimeTests_multi_shape.SWRTFallback.mesh");
  std::error_code error;
  fs::remove(fallbackPath, error);
  fs::remove(rtPath, error);

  expectTrue(virtualgeometry::FallbackMeshFile::save(fallbackPath.string(), expectedFallbackMeshes), "Saving multi-shape fallback mesh file should succeed");
  expectTrue(softwarebvh::SWRTFallbackMeshFile::save(rtPath.string(), expectedRtEntries), "Saving multi-shape SWRT RT file should succeed");

  std::vector<virtualgeometry::FallbackMeshData> loadedFallbackMeshes;
  std::vector<softwarebvh::SWRTFallbackMeshData> loadedRtEntries;
  expectTrue(virtualgeometry::FallbackMeshFile::load(fallbackPath.string(), loadedFallbackMeshes), "Loading multi-shape fallback mesh file should succeed");
  expectTrue(softwarebvh::SWRTFallbackMeshFile::load(rtPath.string(), loadedRtEntries), "Loading multi-shape SWRT RT file should succeed");

  expectEqual(static_cast<uint32_t>(loadedFallbackMeshes.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Fallback mesh file instance count");
  expectEqual(static_cast<uint32_t>(loadedRtEntries.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "SWRT RT file instance count");

  uint32_t loadedFallbackTriangleCount = 0u;
  uint32_t loadedRtTriangleCount = 0u;
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(sourceAsset.shapes.size()); ++shapeIndex)
  {
    const std::string label = "shape[" + std::to_string(shapeIndex) + "]";
    compareFallbackMeshData(loadedFallbackMeshes[shapeIndex], expectedFallbackMeshes[shapeIndex], "Fallback file " + label);
    compareFallbackMeshData(loadedRtEntries[shapeIndex].fallbackMesh, expectedFallbackMeshes[shapeIndex], "SWRT RT file embedded fallback " + label);

    const softwarebvh::CWBVHBuilder::BuildResult expectedBuild = softwarebvh::CWBVHBuilder::build(expectedFallbackMeshes[shapeIndex]);
    compareCwbvhBuildResult(loadedRtEntries[shapeIndex].build, expectedBuild, "SWRT RT file CWBVH build " + label);
    expectEqual(loadedRtEntries[shapeIndex].build.primitiveCount, static_cast<uint32_t>(expectedFallbackMeshes[shapeIndex].indices.size() / 3u), "SWRT RT file primitive count matches triangle count " + label);

    softwarebvh::SWRTFallbackMeshData loadedEntry{};
    expectTrue(softwarebvh::SWRTFallbackMeshFile::loadEntry(rtPath.string(), shapeIndex, loadedEntry), "SWRT RT loadEntry should find " + label);
    compareFallbackMeshData(loadedEntry.fallbackMesh, expectedFallbackMeshes[shapeIndex], "SWRT RT loadEntry fallback " + label);
    compareCwbvhBuildResult(loadedEntry.build, expectedBuild, "SWRT RT loadEntry CWBVH build " + label);

    loadedFallbackTriangleCount += static_cast<uint32_t>(loadedFallbackMeshes[shapeIndex].indices.size() / 3u);
    loadedRtTriangleCount += loadedRtEntries[shapeIndex].build.primitiveCount;
  }

  expectEqual(loadedFallbackTriangleCount, expectedTriangleCount, "Fallback mesh file total triangle count");
  expectEqual(loadedRtTriangleCount, expectedTriangleCount, "SWRT RT file total primitive/triangle count");

  fs::remove(fallbackPath, error);
  fs::remove(rtPath, error);
}

void testSoftwareBlasSceneBridgeBuildsAsyncFromFallbackMesh(rendering::RHI &rhi)
{
  const virtualgeometry::VirtualGeometrySourceAsset sourceAsset = makeMultiShapeSourceAsset();
  const std::vector<virtualgeometry::FallbackMeshData> fallbackMeshes = virtualgeometry::FallbackMeshBuilder::buildOriginalPerShape(sourceAsset);
  expectEqual(static_cast<uint32_t>(fallbackMeshes.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Fallback mesh source shape count");

  const fs::path tempRoot = fs::temp_directory_path() / "SoftwareBVHRuntimeTests_bridge_async_fallback";
  const fs::path virtualGeometryPath = tempRoot / "bridge_async_fallback.virtualgeometry";
  const fs::path fallbackPath = tempRoot / "bridge_async_fallback.lod0.fallback.mesh";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(tempRoot, error);
  expectTrue(!error, "Async fallback scene temp directory should be created");

  virtualgeometry::VirtualGeometryBuildData build = virtualgeometry::VirtualGeometryBuilder::build(sourceAsset);
  virtualgeometry::QuantizationConfig quantizationConfig{};
  quantizationConfig.quantization_factor = 4u;
  quantizationConfig.unit_scale = 100.0f;
  virtualgeometry::VirtualGeometryEncodedData encoded = virtualgeometry::VirtualGeometryCompressor::encode(build, quantizationConfig);
  {
    virtualgeometry::VirtualGeometryFile writer(virtualGeometryPath.string(), true);
    expectTrue(writer.isOpen(), "Async fallback scene virtualgeometry file should open for write");
    expectTrue(writer.write(encoded, build.pages, virtualgeometry::MESHLET_LZ4), "Async fallback scene virtualgeometry file should write");
  }

  std::vector<virtualgeometry::FallbackMeshData> lod0FallbackMeshes = fallbackMeshes;
  for (virtualgeometry::FallbackMeshData &mesh : lod0FallbackMeshes)
  {
    mesh.lodLevel = 0u;
  }
  expectTrue(virtualgeometry::FallbackMeshFile::save(fallbackPath.string(), lod0FallbackMeshes), "Fallback mesh collection should save");

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  virtualgeometry::VirtualGeometryScene scene(&renderGraph, 1024u * 1024u, 16ull * 1024ull * 1024ull);
  scene.registerObjectForStreaming("bridgeObject", virtualGeometryPath.string());
  const virtualgeometry::InstanceId instanceId = scene.instantiateObjectInstance("bridgeObject", math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
  expectTrue(instanceId != virtualgeometry::INVALID_INSTANCE_ID, "Unified SWRT scene should instantiate object");

  BLASManager manager(&renderGraph, blasTestSettings());
  softwarebvh::SoftwareBLASSceneBridge bridge(manager);
  softwarebvh::SoftwareBLASSceneBridge::Settings settings{};
  settings.fallbackLod = 3u;
  settings.primitiveMode = softwarebvh::SoftwareBLASSceneBridge::PrimitiveMode::Triangle;
  bridge.setSettings(settings);
  for (uint32_t attempt = 0u; attempt < 200u; ++attempt)
  {
    bridge.sync(scene);
    if (manager.getLastBuildSummary().blasCount == static_cast<uint32_t>(fallbackMeshes.size()))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto &diagnostics = bridge.getDiagnostics();
  const BLASManager::BuildSummary summary = manager.getLastBuildSummary();
  expectEqual(diagnostics.skippedMissingFallbackMeshCount, 0u, "Bridge should fall back to available fallback mesh LOD");
  expectEqual(summary.blasCount, static_cast<uint32_t>(fallbackMeshes.size()), "Bridge should upload BLAS entries after async fallback builds finish");
  expectTrue(summary.nodeCount > 0u, "Bridge should build non-empty BLAS nodes from fallback mesh shapes");

  TLASManager tlasManager(&renderGraph);
  softwarebvh::SoftwareTLASSceneBridge tlasBridge(tlasManager, &manager);
  tlasBridge.sync(scene);
  const auto &tlasDiagnostics = tlasBridge.getDiagnostics();
  const TLASManager::BuildSummary tlasSummary = tlasManager.getLastBuildSummary();
  expectEqual(tlasDiagnostics.skippedMissingBlasCount, 0u, "TLAS bridge should find BLAS handles built from fallback mesh file");
  expectEqual(tlasSummary.instanceCount, static_cast<uint32_t>(fallbackMeshes.size()), "TLAS bridge should build one instance per source shape");
  expectTrue(tlasSummary.nodeCount > 0u, "TLAS bridge should build non-empty TLAS nodes");

  fs::remove_all(tempRoot, error);
}

void testSponzaSwrtFallbackRtFilePreservesAllInstancesAndTriangles()
{
  const virtualgeometry::VirtualGeometrySourceAsset sourceAsset = loadObjSourceAssetForTest(sponzaAssetPath(), "sponza_swrt_rt_file");
  const std::vector<virtualgeometry::FallbackMeshData> expectedFallbackMeshes = virtualgeometry::FallbackMeshBuilder::buildOriginalPerShape(sourceAsset);
  expectEqual(static_cast<uint32_t>(expectedFallbackMeshes.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Sponza fallback mesh instance count");

  uint32_t expectedTriangleCount = 0u;
  std::vector<softwarebvh::SWRTFallbackMeshData> expectedRtEntries;
  expectedRtEntries.reserve(expectedFallbackMeshes.size());
  for (const virtualgeometry::FallbackMeshData &mesh : expectedFallbackMeshes)
  {
    expectTrue(mesh.indices.size() % 3u == 0u, "Sponza generated fallback mesh should contain whole triangles");
    expectedTriangleCount += static_cast<uint32_t>(mesh.indices.size() / 3u);
    expectedRtEntries.push_back(softwarebvh::SWRTFallbackMeshFile::build(mesh));
  }
  expectTrue(expectedTriangleCount > 0u, "Sponza fallback should contain triangles");

  const fs::path fallbackPath = makeTempPath("SoftwareBVHRuntimeTests_sponza.fallback.mesh");
  const fs::path rtPath = makeTempPath("SoftwareBVHRuntimeTests_sponza.SWRTFallback.mesh");
  std::error_code error;
  fs::remove(fallbackPath, error);
  fs::remove(rtPath, error);

  expectTrue(virtualgeometry::FallbackMeshFile::save(fallbackPath.string(), expectedFallbackMeshes), "Saving Sponza fallback mesh file should succeed");
  expectTrue(softwarebvh::SWRTFallbackMeshFile::save(rtPath.string(), expectedRtEntries), "Saving Sponza SWRT RT file should succeed");

  std::vector<virtualgeometry::FallbackMeshData> loadedFallbackMeshes;
  std::vector<softwarebvh::SWRTFallbackMeshData> loadedRtEntries;
  expectTrue(virtualgeometry::FallbackMeshFile::load(fallbackPath.string(), loadedFallbackMeshes), "Loading Sponza fallback mesh file should succeed");
  expectTrue(softwarebvh::SWRTFallbackMeshFile::load(rtPath.string(), loadedRtEntries), "Loading Sponza SWRT RT file should succeed");

  expectEqual(static_cast<uint32_t>(loadedFallbackMeshes.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Sponza fallback mesh file instance count");
  expectEqual(static_cast<uint32_t>(loadedRtEntries.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Sponza SWRT RT file instance count");

  uint32_t loadedFallbackTriangleCount = 0u;
  uint32_t loadedRtTriangleCount = 0u;
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(sourceAsset.shapes.size()); ++shapeIndex)
  {
    const std::string label = "sponza shape[" + std::to_string(shapeIndex) + "]";
    compareFallbackMeshData(loadedFallbackMeshes[shapeIndex], expectedFallbackMeshes[shapeIndex], "Fallback file " + label);
    compareFallbackMeshData(loadedRtEntries[shapeIndex].fallbackMesh, expectedFallbackMeshes[shapeIndex], "SWRT RT file embedded fallback " + label);
    compareFallbackMeshData(loadedRtEntries[shapeIndex].fallbackMesh, loadedFallbackMeshes[shapeIndex], "SWRT RT file matches fallback file " + label);

    verifyRtBuildCoversAllTriangles(loadedRtEntries[shapeIndex].build, expectedFallbackMeshes[shapeIndex], "SWRT RT file CWBVH build " + label);
    loadedFallbackTriangleCount += static_cast<uint32_t>(loadedFallbackMeshes[shapeIndex].indices.size() / 3u);
    loadedRtTriangleCount += loadedRtEntries[shapeIndex].build.primitiveCount;
  }

  expectEqual(loadedFallbackTriangleCount, expectedTriangleCount, "Sponza fallback mesh file total triangle count");
  expectEqual(loadedRtTriangleCount, expectedTriangleCount, "Sponza SWRT RT file total primitive/triangle count");

  fs::remove(fallbackPath, error);
  fs::remove(rtPath, error);
}

void testBlasRebuildClearsTriangleTransforms(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager manager(&renderGraph, blasTestSettings());

  BLASManager::BuildInput input{};
  input.sourcePath = "tests/rebuild_transform.mesh";
  input.fallbackPath = "tests/rebuild_transform.fallback.mesh";
  input.mesh = makeFrontFacingTriangleMesh(0.0f);
  manager.syncFromCPU({input});

  input.mesh = makeFrontFacingTriangleMesh(2.0f);
  const auto expected = softwarebvh::CWBVHBuilder::build(input.mesh);
  manager.syncFromCPU({input});

  const uint32_t handle = manager.findHandleBySourcePath(input.sourcePath);
  const auto metadata = readBufferValue<BLASManager::TriangleBLASMetadataGPU>(renderGraph, manager.getTriangleMetadataBuffer(), static_cast<uint64_t>(handle) * sizeof(BLASManager::TriangleBLASMetadataGPU));
  const auto transform =
      readBufferValue<BLASManager::TriangleBaryTransformGPU>(renderGraph, manager.getTriangleTransformsBuffer(), static_cast<uint64_t>(metadata.triangleTransformOffset) * sizeof(BLASManager::TriangleBaryTransformGPU));

  for (uint32_t element = 0u; element < 4u; ++element)
  {
    expectNear(transform.row0[element], expected.triangleTransforms[0u].row0[element], "Rebuilt BLAS triangle transform row0");
    expectNear(transform.row1[element], expected.triangleTransforms[0u].row1[element], "Rebuilt BLAS triangle transform row1");
    expectNear(transform.row2[element], expected.triangleTransforms[0u].row2[element], "Rebuilt BLAS triangle transform row2");
  }
}

void testBlasGpuBuildMatchesCpuPackedCwbvh(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager manager(&renderGraph, blasTestSettings());
  const virtualgeometry::FallbackMeshData mesh = makeSeparatedTrianglesMesh();

  BLASManager::BuildInput input{};
  input.sourcePath = "tests/separated_triangles_gpu.mesh";
  input.fallbackPath = "tests/separated_triangles_gpu.fallback.mesh";
  input.mesh = mesh;
  manager.syncFromCPU({input});

  const uint32_t handle = manager.findHandleBySourcePath(input.sourcePath);
  BLASManager::GPUBuildDescriptor descriptor{};
  expectTrue(manager.tryGetGPUBuildDescriptor(handle, &descriptor), "GPU BLAS descriptor lookup should succeed");

  const auto expectedCpuCwbvh =
      readBufferVector<BLASManager::CWBVHNodeGPU>(renderGraph, manager.getGPUCWBVHNodesBuffer(), descriptor.gpuCWBVHNodeCount, static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(BLASManager::CWBVHNodeGPU));
  const auto expectedLeafRemap = readBufferVector<uint32_t>(renderGraph, manager.getGPULeafRemapBuffer(), descriptor.primitiveCount, static_cast<uint64_t>(descriptor.gpuLeafRemapOffset) * sizeof(uint32_t));

  auto *preparePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUPreparePass>("SoftwareBVHTests.prepare", 0u, manager, handle);
  auto *finalizePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUFinalizePass>("SoftwareBVHTests.finalize", 1u, manager, handle);
  runGraph(renderGraph);

  const auto gpuAabbs = readBufferVector<BLASManager::GPUBuildAABBGPU>(renderGraph, manager.getGPUBuildAabbsBuffer(), descriptor.primitiveCount);
  expectEqual(static_cast<uint32_t>(gpuAabbs.size()), descriptor.primitiveCount, "GPU AABB count");
  for (uint32_t primitiveIndex = 0u; primitiveIndex < descriptor.primitiveCount; ++primitiveIndex)
  {
    const auto expectedBounds = triangleBounds(mesh, primitiveIndex);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      expectNear(gpuAabbs[primitiveIndex].minPoint[axis], expectedBounds.minPoint[axis], "GPU triangle AABB min");
      expectNear(gpuAabbs[primitiveIndex].maxPoint[axis], expectedBounds.maxPoint[axis], "GPU triangle AABB max");
    }
  }

  const auto actualGpuCwbvh =
      readBufferVector<BLASManager::CWBVHNodeGPU>(renderGraph, manager.getGPUCWBVHNodesBuffer(), descriptor.gpuCWBVHNodeCount, static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(BLASManager::CWBVHNodeGPU));
  const auto actualLeafRemap = readBufferVector<uint32_t>(renderGraph, manager.getGPULeafRemapBuffer(), descriptor.primitiveCount, static_cast<uint64_t>(descriptor.gpuLeafRemapOffset) * sizeof(uint32_t));

  expectEqual(static_cast<uint32_t>(actualGpuCwbvh.size()), static_cast<uint32_t>(expectedCpuCwbvh.size()), "GPU CWBVH node count should match CPU packed count");
  for (uint32_t nodeIndex = 0u; nodeIndex < actualGpuCwbvh.size(); ++nodeIndex)
  {
    expectTrue(std::memcmp(&actualGpuCwbvh[nodeIndex], &expectedCpuCwbvh[nodeIndex], sizeof(BLASManager::CWBVHNodeGPU)) == 0, "GPU CWBVH node should exactly match CPU packed node " + std::to_string(nodeIndex));
  }

  expectEqual(static_cast<uint32_t>(actualLeafRemap.size()), static_cast<uint32_t>(expectedLeafRemap.size()), "GPU leaf remap size");
  for (uint32_t remapIndex = 0u; remapIndex < actualLeafRemap.size(); ++remapIndex)
  {
    expectEqual(actualLeafRemap[remapIndex], expectedLeafRemap[remapIndex], "GPU leaf remap[" + std::to_string(remapIndex) + "]");
  }

  if (descriptor.gpuCWBVHNodeCount > 0u)
  {
    const auto leafSlots = collectPackedLeafSlots(actualGpuCwbvh.front(), actualLeafRemap);
    expectEqual(static_cast<uint32_t>(leafSlots.size()), descriptor.primitiveCount, "Packed CWBVH root should expose all primitive leaves");

    for (const auto &[childSlot, primitiveIndex] : leafSlots)
    {
      const virtualgeometry::AABB decodedLeaf = decodePackedChildBounds(actualGpuCwbvh.front(), childSlot);
      const virtualgeometry::AABB expectedLeaf = triangleBounds(mesh, primitiveIndex);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        expectTrue(decodedLeaf.minPoint[axis] <= expectedLeaf.minPoint[axis] + kEpsilon, "Packed CWBVH child min should enclose expected triangle bounds");
        expectTrue(decodedLeaf.maxPoint[axis] >= expectedLeaf.maxPoint[axis] - kEpsilon, "Packed CWBVH child max should enclose expected triangle bounds");
      }
    }
  }

  delete finalizePass;
  delete preparePass;
}

void testBlasGpuBuildHandlesLeafBeforeInternalChild(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  const virtualgeometry::FallbackMeshData mesh = makeLeafBeforeInternalChildTrianglesMesh();
  BLASManager manager(&renderGraph, blasTestSettingsForMesh(mesh));

  BLASManager::BuildInput input{};
  input.sourcePath = "tests/leaf_before_internal_gpu.mesh";
  input.fallbackPath = "tests/leaf_before_internal_gpu.fallback.mesh";
  input.mesh = mesh;
  manager.syncFromCPU({input});

  const uint32_t handle = manager.findHandleBySourcePath(input.sourcePath);
  BLASManager::GPUBuildDescriptor descriptor{};
  expectTrue(manager.tryGetGPUBuildDescriptor(handle, &descriptor), "Leaf-before-internal BLAS descriptor lookup should succeed");

  auto *preparePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUPreparePass>("SoftwareBVHTests.prepareLeafBeforeInternal", 0u, manager, handle);
  auto *finalizePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUFinalizePass>("SoftwareBVHTests.finalizeLeafBeforeInternal", 1u, manager, handle);
  runGraph(renderGraph);

  const auto actualGpuCwbvh =
      readBufferVector<BLASManager::CWBVHNodeGPU>(renderGraph, manager.getGPUCWBVHNodesBuffer(), descriptor.gpuCWBVHNodeCount, static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(BLASManager::CWBVHNodeGPU));
  const auto actualLeafRemap = readBufferVector<uint32_t>(renderGraph, manager.getGPULeafRemapBuffer(), descriptor.primitiveCount, static_cast<uint64_t>(descriptor.gpuLeafRemapOffset) * sizeof(uint32_t));

  validatePackedInternalChildLinksWithoutNodeRefs(actualGpuCwbvh, "Leaf-before-internal BLAS GPU build");
  validateBlasPackedLeafCoverageWithoutNodeRefs(actualGpuCwbvh, actualLeafRemap, mesh, "Leaf-before-internal BLAS GPU build");
  expectTrue(
      std::any_of(
          actualGpuCwbvh.begin(),
          actualGpuCwbvh.end(),
          [](const BLASManager::CWBVHNodeGPU &node)
          {
            return packedNodeHasLeafBeforeInternalChild(node);
          }),
      "Leaf-before-internal BLAS GPU build should contain a packed node with leaf slots before internal slots");

  delete finalizePass;
  delete preparePass;
}

void testTlasRebuildAndRefit(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  TLASManager manager(&renderGraph, tlasTestSettings());
  const auto localBounds = meshBounds(makeUnitCubeMesh());

  std::vector<TLASManager::InstanceBuildInput> instances(2u);
  instances[0u].instanceId = 10u;
  instances[0u].blasHandle = 3u;
  instances[0u].transform = makeTranslation(math::Vec3f(-2.0f, 0.0f, -5.0f));
  instances[0u].previousTransform = instances[0u].transform;
  instances[0u].worldBounds = translateBounds(localBounds, math::Vec3f(-2.0f, 0.0f, -5.0f));
  instances[0u].sourceName = "cubeA";
  instances[0u].sourcePath = "tests/cubeA";

  instances[1u].instanceId = 11u;
  instances[1u].blasHandle = 3u;
  instances[1u].transform = makeTranslation(math::Vec3f(2.0f, 0.0f, -5.0f));
  instances[1u].previousTransform = instances[1u].transform;
  instances[1u].worldBounds = translateBounds(localBounds, math::Vec3f(2.0f, 0.0f, -5.0f));
  instances[1u].sourceName = "cubeB";
  instances[1u].sourcePath = "tests/cubeB";

  const TLASManager::BuildSummary rebuildSummary = manager.rebuild(instances, 2u);
  expectEqual(rebuildSummary.instanceCount, 2u, "TLAS rebuild instance count");
  expectEqual(rebuildSummary.nodeCount, 3u, "TLAS rebuild node count");
  expectTrue(rebuildSummary.rebuilt, "TLAS rebuild should mark rebuilt");
  expectEqual(rebuildSummary.buildRevision, 1u, "TLAS rebuild revision");

  const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), rebuildSummary.nodeCount);
  expectEqual(static_cast<uint32_t>(canonicalNodes.size()), 3u, "TLAS canonical node readback count");
  validateTlasCanonicalHierarchy(canonicalNodes, rebuildSummary.rootNodeIndex, instances, "TLAS rebuild canonical hierarchy");

  virtualgeometry::AABB expectedRootBounds{};
  expectedRootBounds.minPoint[0] = -2.5f;
  expectedRootBounds.minPoint[1] = -0.5f;
  expectedRootBounds.minPoint[2] = -5.5f;
  expectedRootBounds.maxPoint[0] = 2.5f;
  expectedRootBounds.maxPoint[1] = 0.5f;
  expectedRootBounds.maxPoint[2] = -4.5f;

  virtualgeometry::AABB actualRootBounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    actualRootBounds.minPoint[axis] = canonicalNodes[rebuildSummary.rootNodeIndex].minPoint[axis];
    actualRootBounds.maxPoint[axis] = canonicalNodes[rebuildSummary.rootNodeIndex].maxPoint[axis];
  }
  compareBounds(actualRootBounds, expectedRootBounds, "TLAS root bounds after rebuild");

  const auto leafInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getLeafInstanceIndicesBuffer(), manager.getLeafInstanceCount());
  std::set<uint32_t> observedLeafInstances(leafInstanceIndices.begin(), leafInstanceIndices.end());
  expectTrue(observedLeafInstances == std::set<uint32_t>{0u, 1u}, "TLAS leaf instance indices should cover both compacted instances");
  expectTrue(manager.getCwbvhNodeCount() > 0u, "TLAS rebuild should produce at least one packed CWBVH node");

  const auto rebuildCounters = readBufferValue<TLASManager::CountersGPU>(renderGraph, manager.getCountersBuffer());
  const uint32_t rebuildSortCount = readBufferValue<uint32_t>(renderGraph, manager.getBuildSortCountBuffer());
  const auto rebuildSortDispatchArgs = readBufferVector<TLASManager::DispatchIndirectArgsGPU>(renderGraph, manager.getBuildSortIndirectArgsBuffer(), 6u);
  expectEqual(rebuildCounters.instanceCount, 2u, "TLAS counters instance count");
  expectEqual(rebuildCounters.nodeCount, 3u, "TLAS counters node count");
  expectEqual(rebuildCounters.buildRevision, 1u, "TLAS counters build revision");
  expectEqual(rebuildCounters.refitRevision, 0u, "TLAS counters refit revision before refit");
  expectEqual(rebuildCounters.buildRequested, 1u, "TLAS counters build requested");
  expectEqual(rebuildCounters.refitRequested, 0u, "TLAS counters refit requested before refit");
  expectEqual(rebuildSortCount, 2u, "TLAS rebuild sort count should include active instances");
  expectEqual(rebuildSortDispatchArgs[0u].groupCountX, 1u, "TLAS rebuild sort clear dispatch count");
  expectEqual(rebuildSortDispatchArgs[1u].groupCountX, 1u, "TLAS rebuild sort count dispatch count");
  expectEqual(rebuildSortDispatchArgs[2u].groupCountX, 16u, "TLAS rebuild sort reduce dispatch count");
  expectEqual(rebuildSortDispatchArgs[3u].groupCountX, 1u, "TLAS rebuild sort scan dispatch count");
  expectEqual(rebuildSortDispatchArgs[4u].groupCountX, 16u, "TLAS rebuild sort scan-add dispatch count");
  expectEqual(rebuildSortDispatchArgs[5u].groupCountX, 1u, "TLAS rebuild sort scatter dispatch count");

  instances[1u].previousTransform = instances[1u].transform;
  instances[1u].transform = makeTranslation(math::Vec3f(6.0f, 0.0f, -5.0f));
  instances[1u].worldBounds = translateBounds(localBounds, math::Vec3f(6.0f, 0.0f, -5.0f));

  const TLASManager::BuildSummary refitSummary = manager.refit(instances, {1u});
  expectTrue(refitSummary.refitted, "TLAS refit should mark refitted");
  expectTrue(!refitSummary.rebuilt, "TLAS refit should not mark rebuilt");
  expectEqual(refitSummary.dirtyInstanceCount, 1u, "TLAS refit dirty instance count");
  expectEqual(refitSummary.buildRevision, 1u, "TLAS refit should preserve build revision");
  expectEqual(refitSummary.refitRevision, 1u, "TLAS refit revision");

  const auto canonicalNodesAfterRefit = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), refitSummary.nodeCount);
  validateTlasCanonicalHierarchy(canonicalNodesAfterRefit, refitSummary.rootNodeIndex, instances, "TLAS refit canonical hierarchy");
  virtualgeometry::AABB expectedRefitRootBounds = expectedRootBounds;
  expectedRefitRootBounds.maxPoint[0] = 6.5f;
  virtualgeometry::AABB actualRefitRootBounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    actualRefitRootBounds.minPoint[axis] = canonicalNodesAfterRefit[refitSummary.rootNodeIndex].minPoint[axis];
    actualRefitRootBounds.maxPoint[axis] = canonicalNodesAfterRefit[refitSummary.rootNodeIndex].maxPoint[axis];
  }
  compareBounds(actualRefitRootBounds, expectedRefitRootBounds, "TLAS root bounds after refit");

  const auto refitCounters = readBufferValue<TLASManager::CountersGPU>(renderGraph, manager.getCountersBuffer());
  const uint32_t refitSortCount = readBufferValue<uint32_t>(renderGraph, manager.getBuildSortCountBuffer());
  const auto refitSortDispatchArgs = readBufferVector<TLASManager::DispatchIndirectArgsGPU>(renderGraph, manager.getBuildSortIndirectArgsBuffer(), 6u);
  expectEqual(refitCounters.buildRevision, 1u, "TLAS counters build revision after refit");
  expectEqual(refitCounters.refitRevision, 1u, "TLAS counters refit revision after refit");
  expectEqual(refitCounters.dirtyInstanceCount, 1u, "TLAS counters dirty instance count after refit");
  expectEqual(refitCounters.buildRequested, 0u, "TLAS counters build requested after refit");
  expectEqual(refitCounters.refitRequested, 1u, "TLAS counters refit requested after refit");
  expectEqual(refitSortCount, 0u, "TLAS refit should not request rebuild sort work");
  for (const TLASManager::DispatchIndirectArgsGPU &args : refitSortDispatchArgs)
  {
    expectEqual(args.groupCountX, 0u, "TLAS refit should zero rebuild sort indirect work");
  }

  const TLASManager::BuildSummary cleanSummary = manager.markClean();
  expectTrue(!cleanSummary.rebuilt, "TLAS clean frame should not mark rebuilt");
  expectTrue(!cleanSummary.refitted, "TLAS clean frame should not mark refitted");
  const auto cleanCounters = readBufferValue<TLASManager::CountersGPU>(renderGraph, manager.getCountersBuffer());
  const uint32_t cleanSortCount = readBufferValue<uint32_t>(renderGraph, manager.getBuildSortCountBuffer());
  const auto cleanSortDispatchArgs = readBufferVector<TLASManager::DispatchIndirectArgsGPU>(renderGraph, manager.getBuildSortIndirectArgsBuffer(), 6u);
  expectEqual(cleanCounters.buildRequested, 0u, "TLAS counters build requested after clean");
  expectEqual(cleanCounters.refitRequested, 0u, "TLAS counters refit requested after clean");
  expectEqual(cleanSortCount, 0u, "TLAS clean frame should not request rebuild sort work");
  for (const TLASManager::DispatchIndirectArgsGPU &args : cleanSortDispatchArgs)
  {
    expectEqual(args.groupCountX, 0u, "TLAS clean frame should zero rebuild sort indirect work");
  }
}

void testTlasRefitSurvivesInputReorder(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  TLASManager manager(&renderGraph, tlasTestSettings());
  const auto localBounds = meshBounds(makeUnitCubeMesh());

  std::vector<TLASManager::InstanceBuildInput> instances(2u);
  instances[0u].instanceId = 10u;
  instances[0u].blasHandle = 3u;
  instances[0u].transform = makeTranslation(math::Vec3f(-2.0f, 0.0f, -5.0f));
  instances[0u].previousTransform = instances[0u].transform;
  instances[0u].worldBounds = translateBounds(localBounds, math::Vec3f(-2.0f, 0.0f, -5.0f));
  instances[0u].sourceName = "cubeA";
  instances[0u].sourcePath = "tests/cubeA";

  instances[1u].instanceId = 11u;
  instances[1u].blasHandle = 3u;
  instances[1u].transform = makeTranslation(math::Vec3f(2.0f, 0.0f, -5.0f));
  instances[1u].previousTransform = instances[1u].transform;
  instances[1u].worldBounds = translateBounds(localBounds, math::Vec3f(2.0f, 0.0f, -5.0f));
  instances[1u].sourceName = "cubeB";
  instances[1u].sourcePath = "tests/cubeB";

  manager.rebuild(instances, 2u);

  std::swap(instances[0u], instances[1u]);
  instances[0u].previousTransform = instances[0u].transform;
  instances[0u].transform = makeTranslation(math::Vec3f(6.0f, 0.0f, -5.0f));
  instances[0u].worldBounds = translateBounds(localBounds, math::Vec3f(6.0f, 0.0f, -5.0f));

  const TLASManager::BuildSummary refitSummary = manager.refit(instances, {0u});
  expectTrue(refitSummary.refitted, "TLAS reordered dirty instance should use refit");
  expectTrue(!refitSummary.rebuilt, "TLAS reordered dirty instance should not rebuild");
  expectEqual(refitSummary.buildRevision, 1u, "TLAS reordered refit should preserve build revision");
  expectEqual(refitSummary.refitRevision, 1u, "TLAS reordered refit revision");

  const auto canonicalNodesAfterRefit = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), refitSummary.nodeCount);
  validateTlasCanonicalHierarchy(canonicalNodesAfterRefit, refitSummary.rootNodeIndex, instances, "TLAS reordered refit canonical hierarchy");
  virtualgeometry::AABB expectedRefitRootBounds{};
  expectedRefitRootBounds.minPoint[0] = -2.5f;
  expectedRefitRootBounds.minPoint[1] = -0.5f;
  expectedRefitRootBounds.minPoint[2] = -5.5f;
  expectedRefitRootBounds.maxPoint[0] = 6.5f;
  expectedRefitRootBounds.maxPoint[1] = 0.5f;
  expectedRefitRootBounds.maxPoint[2] = -4.5f;

  virtualgeometry::AABB actualRefitRootBounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    actualRefitRootBounds.minPoint[axis] = canonicalNodesAfterRefit[refitSummary.rootNodeIndex].minPoint[axis];
    actualRefitRootBounds.maxPoint[axis] = canonicalNodesAfterRefit[refitSummary.rootNodeIndex].maxPoint[axis];
  }
  compareBounds(actualRefitRootBounds, expectedRefitRootBounds, "TLAS reordered root bounds after refit");
}

void testTlasPackedBoundsEncloseHierarchy(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  TLASManager manager(&renderGraph, tlasTestSettings());
  const auto localBounds = meshBounds(makeUnitCubeMesh());

  std::vector<TLASManager::InstanceBuildInput> instances(12u);
  for (uint32_t i = 0u; i < static_cast<uint32_t>(instances.size()); ++i)
  {
    const float x = static_cast<float>(i % 4u) * 255.0f - 384.0f;
    const float y = static_cast<float>(i / 4u) * 127.5f - 128.0f;
    const float z = -1024.0f + static_cast<float>(i % 3u) * 255.0f;
    const math::Vec3f translation(x, y, z);
    instances[i].instanceId = 1000u + i;
    instances[i].blasHandle = 3u;
    instances[i].transform = makeTranslation(translation);
    instances[i].previousTransform = instances[i].transform;
    instances[i].worldBounds = translateBounds(localBounds, translation);
    instances[i].sourceName = "quantizedCube" + std::to_string(i);
    instances[i].sourcePath = "tests/quantizedCube" + std::to_string(i);
  }

  const TLASManager::BuildSummary summary = manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
  expectTrue(summary.nodeCount > instances.size(), "TLAS quantized hierarchy should include internal nodes");
  expectTrue(manager.getCwbvhNodeCount() > 1u, "TLAS quantized hierarchy should produce multiple packed nodes");

  const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), summary.nodeCount);
  const auto packedNodes = readBufferVector<TLASManager::CWBVHNodeGPU>(renderGraph, manager.getCwbvhNodesBuffer(), manager.getCwbvhNodeCount());
  const auto leafInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getLeafInstanceIndicesBuffer(), manager.getLeafInstanceCount());
  validateTlasCanonicalHierarchy(canonicalNodes, summary.rootNodeIndex, instances, "CPU TLAS quantized canonical hierarchy");
  expectTrue(manager.getBvh4NodeCount() > 0u, "CPU TLAS quantized hierarchy should build SAH BVH4 nodes");
  expectTrue(manager.getCwbvhNodeCount() <= manager.getBvh4NodeCount(), "CPU TLAS CWBVH should be transformed from BVH4 without expanding node count");
  validatePackedInternalChildLinksWithoutNodeRefs(packedNodes, "CPU TLAS quantized packed child links");
  validateTlasPackedLeafCoverageWithoutNodeRefs(packedNodes, leafInstanceIndices, instances, "CPU TLAS quantized packed leaf coverage");
  validateTlasPackedRayReachabilityWithoutNodeRefs(packedNodes, leafInstanceIndices, instances, "CPU TLAS quantized packed ray reachability");
}

void testTlasPartialSubtreeRebuild(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  TLASManager::Settings settings = tlasTestSettings();
  settings.partialRebuildEnabled = true;
  settings.partialRebuildMinLeafCount = 4u;
  settings.partialRebuildDirtyRatioThreshold = 0.5f;
  settings.partialRebuildSahGrowthThreshold = 4.0f;
  TLASManager manager(&renderGraph, settings);
  const auto localBounds = meshBounds(makeUnitCubeMesh());

  std::vector<TLASManager::InstanceBuildInput> instances(8u);
  const std::array<float, 8u> initialX = {-10.0f, -8.0f, -6.0f, -4.0f, 4.0f, 6.0f, 8.0f, 10.0f};
  for (uint32_t i = 0u; i < static_cast<uint32_t>(instances.size()); ++i)
  {
    const math::Vec3f translation(initialX[i], 0.0f, -5.0f);
    instances[i].instanceId = 200u + i;
    instances[i].blasHandle = 7u;
    instances[i].transform = makeTranslation(translation);
    instances[i].previousTransform = instances[i].transform;
    instances[i].worldBounds = translateBounds(localBounds, translation);
    instances[i].sourceName = "partialCube" + std::to_string(i);
    instances[i].sourcePath = "tests/partialCube" + std::to_string(i);
  }

  const TLASManager::BuildSummary rebuildSummary = manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
  expectTrue(rebuildSummary.rebuilt, "TLAS partial test initial build should rebuild");
  expectEqual(rebuildSummary.buildRevision, 1u, "TLAS partial test initial build revision");

  instances[0u].previousTransform = instances[0u].transform;
  instances[0u].transform = makeTranslation(math::Vec3f(-12.0f, 0.0f, -5.0f));
  instances[0u].worldBounds = translateBounds(localBounds, math::Vec3f(-12.0f, 0.0f, -5.0f));
  instances[1u].previousTransform = instances[1u].transform;
  instances[1u].transform = makeTranslation(math::Vec3f(-11.0f, 0.0f, -5.0f));
  instances[1u].worldBounds = translateBounds(localBounds, math::Vec3f(-11.0f, 0.0f, -5.0f));

  const TLASManager::BuildSummary refitSummary = manager.refit(instances, {0u, 1u});
  expectTrue(refitSummary.refitted, "TLAS dirty clustered instances should refit");
  expectTrue(!refitSummary.rebuilt, "TLAS dirty clustered instances should not full rebuild");
  expectTrue(refitSummary.partialRebuilt, "TLAS dirty clustered instances should partially rebuild a subtree");
  expectEqual(refitSummary.partialRebuildRootCount, 1u, "TLAS dirty clustered instances partial rebuild root count");
  expectEqual(refitSummary.buildRevision, 1u, "TLAS partial rebuild should preserve full build revision");
  expectEqual(refitSummary.refitRevision, 1u, "TLAS partial rebuild should advance refit revision");

  virtualgeometry::AABB expectedRoot = instances[0u].worldBounds;
  for (uint32_t i = 1u; i < static_cast<uint32_t>(instances.size()); ++i)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      expectedRoot.minPoint[axis] = std::min(expectedRoot.minPoint[axis], instances[i].worldBounds.minPoint[axis]);
      expectedRoot.maxPoint[axis] = std::max(expectedRoot.maxPoint[axis], instances[i].worldBounds.maxPoint[axis]);
    }
  }

  const auto canonicalNodesAfterPartial = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), refitSummary.nodeCount);
  validateTlasCanonicalHierarchy(canonicalNodesAfterPartial, refitSummary.rootNodeIndex, instances, "TLAS partial rebuild canonical hierarchy");
  virtualgeometry::AABB actualRoot{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    actualRoot.minPoint[axis] = canonicalNodesAfterPartial[refitSummary.rootNodeIndex].minPoint[axis];
    actualRoot.maxPoint[axis] = canonicalNodesAfterPartial[refitSummary.rootNodeIndex].maxPoint[axis];
  }
  compareBounds(actualRoot, expectedRoot, "TLAS partial rebuild root bounds");

  const auto leafInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getLeafInstanceIndicesBuffer(), manager.getLeafInstanceCount());
  std::set<uint32_t> observedLeafInstances(leafInstanceIndices.begin(), leafInstanceIndices.end());
  expectTrue(observedLeafInstances == std::set<uint32_t>{0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}, "TLAS partial rebuild leaf remap should still cover all instances");
  const auto packedNodes = readBufferVector<TLASManager::CWBVHNodeGPU>(renderGraph, manager.getCwbvhNodesBuffer(), manager.getCwbvhNodeCount());
  validatePackedInternalChildLinksWithoutNodeRefs(packedNodes, "TLAS partial rebuild packed child links");
  validateTlasPackedLeafCoverageWithoutNodeRefs(packedNodes, leafInstanceIndices, instances, "TLAS partial rebuild packed leaf coverage");
  validateTlasPackedRayReachabilityWithoutNodeRefs(packedNodes, leafInstanceIndices, instances, "TLAS partial rebuild packed ray reachability");
}

void testSponzaTlasBuildIndices(rendering::RHI &rhi)
{
  const virtualgeometry::VirtualGeometrySourceAsset sourceAsset = loadObjSourceAssetForTest(sponzaAssetPath(), "sponza_tlas_build");
  const std::vector<virtualgeometry::FallbackMeshData> fallbackMeshes = virtualgeometry::FallbackMeshBuilder::buildOriginalPerShape(sourceAsset);
  expectEqual(static_cast<uint32_t>(fallbackMeshes.size()), static_cast<uint32_t>(sourceAsset.shapes.size()), "Sponza TLAS fallback mesh instance count");

  std::vector<TLASManager::InstanceBuildInput> instances;
  std::vector<BLASManager::BuildInput> blasInputs;
  instances.reserve(fallbackMeshes.size());
  blasInputs.reserve(fallbackMeshes.size());
  uint32_t totalVertexCount = 0u;
  uint32_t totalIndexCount = 0u;
  uint32_t totalPrimitiveCount = 0u;
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(fallbackMeshes.size()); ++shapeIndex)
  {
    const virtualgeometry::FallbackMeshData &mesh = fallbackMeshes[shapeIndex];
    expectTrue(mesh.indices.size() % 3u == 0u, "Sponza TLAS fallback mesh should contain whole triangles[" + std::to_string(shapeIndex) + "]");
    if (mesh.vertices.empty() || mesh.indices.empty())
      continue;

    const virtualgeometry::AABB bounds = meshBounds(mesh);
    expectTrue(isValidTestBounds(bounds), "Sponza TLAS fallback mesh bounds should be valid[" + std::to_string(shapeIndex) + "]");

    TLASManager::InstanceBuildInput instance{};
    instance.instanceId = 100000u + shapeIndex;
    instance.blasHandle = shapeIndex;
    instance.transform = math::Mat4f::identity();
    instance.previousTransform = instance.transform;
    instance.worldBounds = bounds;
    instance.sourceName = "sponza_shape_" + std::to_string(shapeIndex);
    instance.sourcePath = sponzaAssetPath().string() + "::shape" + std::to_string(shapeIndex);
    instances.push_back(instance);

    BLASManager::BuildInput blasInput{};
    blasInput.sourcePath = instance.sourcePath;
    blasInput.fallbackPath = instance.sourcePath + ".fallback";
    blasInput.mesh = mesh;
    blasInputs.push_back(std::move(blasInput));
    totalVertexCount += static_cast<uint32_t>(mesh.vertices.size());
    totalIndexCount += static_cast<uint32_t>(mesh.indices.size());
    totalPrimitiveCount += static_cast<uint32_t>(mesh.indices.size() / 3u);
  }
  expectTrue(!instances.empty(), "Sponza TLAS should contain valid instances");

  TLASManager::Settings settings = tlasTestSettings();
  settings.maxInstances = std::max<uint32_t>(static_cast<uint32_t>(instances.size()), settings.maxInstances);

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 128u, .maxFramesInFlight = 1u});
  BLASManager::Settings blasSettings{};
  blasSettings.maxBlases = static_cast<uint32_t>(blasInputs.size());
  blasSettings.maxVertices = std::max<uint32_t>(totalVertexCount + 16u, 128u);
  blasSettings.maxIndices = std::max<uint32_t>(totalIndexCount + 32u, 256u);
  blasSettings.maxNodes = std::max<uint32_t>(totalPrimitiveCount * 2u + static_cast<uint32_t>(blasInputs.size()), 128u);
  blasSettings.maxPrimitives = std::max<uint32_t>(totalPrimitiveCount + static_cast<uint32_t>(blasInputs.size()), 64u);
  blasSettings.maxPackedCWBVHNodes = std::max<uint32_t>(totalPrimitiveCount + static_cast<uint32_t>(blasInputs.size()), 128u);
  blasSettings.maxRbvhDepthMipTexels = 64u;
  blasSettings.frameLocalBuffers = false;
  BLASManager blasManager(&renderGraph, blasSettings);
  const BLASManager::BuildSummary blasSummary = blasManager.syncFromCPU(blasInputs);
  expectTrue(blasSummary.rebuilt, "Sponza BLAS CPU build should rebuild");
  expectEqual(blasSummary.blasCount, static_cast<uint32_t>(blasInputs.size()), "Sponza BLAS CPU build blas count");
  expectEqual(blasSummary.primitiveCount, totalPrimitiveCount, "Sponza BLAS CPU build primitive count");
  for (uint32_t blasIndex = 0u; blasIndex < static_cast<uint32_t>(blasInputs.size()); ++blasIndex)
  {
    const uint32_t handle = blasManager.findHandleBySourcePath(blasInputs[blasIndex].sourcePath);
    expectTrue(handle != BLASManager::INVALID_BLAS_HANDLE, "Sponza BLAS handle should be valid[" + std::to_string(blasIndex) + "]");
    BLASManager::GPUBuildDescriptor descriptor{};
    expectTrue(blasManager.tryGetGPUBuildDescriptor(handle, &descriptor), "Sponza BLAS descriptor should be available[" + std::to_string(blasIndex) + "]");
    const auto blasPackedNodes = readBufferVector<BLASManager::CWBVHNodeGPU>(
        renderGraph, blasManager.getGPUCWBVHNodesBuffer(), descriptor.gpuCWBVHNodeCount, static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(BLASManager::CWBVHNodeGPU));
    const auto blasLeafRemap = readBufferVector<uint32_t>(renderGraph, blasManager.getGPULeafRemapBuffer(), descriptor.primitiveCount, static_cast<uint64_t>(descriptor.gpuLeafRemapOffset) * sizeof(uint32_t));
    validateBlasPackedLeafCoverageWithoutNodeRefs(blasPackedNodes, blasLeafRemap, blasInputs[blasIndex].mesh, "Sponza BLAS CPU packed leaf coverage[" + std::to_string(blasIndex) + "]");
  }

  TLASManager manager(&renderGraph, settings);

  const TLASManager::BuildSummary summary = manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
  const uint32_t expectedNodeCount = static_cast<uint32_t>(instances.size() * 2u - 1u);
  expectTrue(summary.rebuilt, "Sponza TLAS rebuild should mark rebuilt");
  expectEqual(summary.instanceCount, static_cast<uint32_t>(instances.size()), "Sponza TLAS rebuild instance count");
  expectEqual(summary.nodeCount, expectedNodeCount, "Sponza TLAS rebuild canonical node count");
  expectEqual(manager.getCanonicalNodeCount(), expectedNodeCount, "Sponza TLAS manager canonical node count");
  expectEqual(manager.getLeafInstanceCount(), static_cast<uint32_t>(instances.size()), "Sponza TLAS manager leaf remap count");
  expectTrue(manager.getCwbvhNodeCount() > 0u, "Sponza TLAS CPU build should produce packed CWBVH nodes");

  const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), summary.nodeCount);
  const auto packedNodes = readBufferVector<TLASManager::CWBVHNodeGPU>(renderGraph, manager.getCwbvhNodesBuffer(), manager.getCwbvhNodeCount());
  const auto leafInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getLeafInstanceIndicesBuffer(), manager.getLeafInstanceCount());
  validateTlasCanonicalHierarchy(canonicalNodes, summary.rootNodeIndex, instances, "Sponza TLAS CPU canonical hierarchy");
  expectTrue(manager.getBvh4NodeCount() > 0u, "Sponza TLAS CPU build should produce SAH BVH4 nodes");
  expectTrue(manager.getCwbvhNodeCount() <= manager.getBvh4NodeCount(), "Sponza TLAS CWBVH should be transformed from BVH4 without expanding node count");
  validatePackedInternalChildLinksWithoutNodeRefs(packedNodes, "Sponza TLAS CPU packed child links");
  validateTlasPackedLeafCoverageWithoutNodeRefs(packedNodes, leafInstanceIndices, instances, "Sponza TLAS CPU packed leaf coverage");
  validateTlasPackedRayReachabilityWithoutNodeRefs(packedNodes, leafInstanceIndices, instances, "Sponza TLAS CPU packed ray reachability");
  validateTlasLeafInstanceRemap(leafInstanceIndices, static_cast<uint32_t>(instances.size()), "Sponza TLAS CPU");

  auto passes = registerTlasGpuRebuildPasses(renderGraph, manager, 0u, "SoftwareBVHTests.sponzaTlasGpuBuild");
  runGraphWithSortTimers(renderGraph, passes);

  expectEqual(manager.getCanonicalNodeCount(), expectedNodeCount, "Sponza TLAS GPU manager canonical node count");
  validateGenericTlasGpuHlbvhLeaves(renderGraph, manager, instances, "Sponza TLAS GPU");
  validateGenericTlasGpuHlbvhTopology(renderGraph, manager, instances, "Sponza TLAS GPU");
  const GenericCwbvhReadback gpuCwbvh = readGenericTlasGpuCwbvhReadback(renderGraph, manager, static_cast<uint32_t>(instances.size()), "Sponza TLAS GPU");
  validatePackedInternalChildLinksWithoutNodeRefs(gpuCwbvh.nodes, "Sponza TLAS GPU packed child links");
  validateTlasPackedLeafCoverageWithoutNodeRefs(gpuCwbvh.nodes, gpuCwbvh.leafInstanceIndices, instances, "Sponza TLAS GPU packed leaf coverage");
  validateTlasPackedRayReachabilityWithoutNodeRefs(gpuCwbvh.nodes, gpuCwbvh.leafInstanceIndices, instances, "Sponza TLAS GPU packed ray reachability");
  validateTlasLeafInstanceRemap(gpuCwbvh.leafInstanceIndices, static_cast<uint32_t>(instances.size()), "Sponza TLAS GPU");

  const auto counters = readBufferValue<TLASManager::CountersGPU>(renderGraph, manager.getCountersBuffer());
  expectEqual(counters.instanceCount, static_cast<uint32_t>(instances.size()), "Sponza TLAS counters instance count");
  expectEqual(counters.nodeCount, expectedNodeCount, "Sponza TLAS counters node count");
  expectEqual(counters.buildRevision, 1u, "Sponza TLAS counters build revision");

  deletePasses(passes);
}

void testTlasGpuBuildHandlesLeafBeforeInternalChild(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  TLASManager manager(&renderGraph, tlasTestSettings());
  const auto localBounds = meshBounds(makeUnitCubeMesh());
  const auto translations = makeLeafBeforeInternalChildTranslations();

  std::vector<TLASManager::InstanceBuildInput> instances(translations.size());
  for (uint32_t i = 0u; i < static_cast<uint32_t>(instances.size()); ++i)
  {
    instances[i].instanceId = 500u + i;
    instances[i].blasHandle = 3u;
    instances[i].transform = makeTranslation(translations[i]);
    instances[i].previousTransform = instances[i].transform;
    instances[i].worldBounds = translateBounds(localBounds, translations[i]);
    instances[i].sourceName = "leafBeforeInternalCube" + std::to_string(i);
    instances[i].sourcePath = "tests/leafBeforeInternalCube" + std::to_string(i);
  }

  const TLASManager::BuildSummary summary = manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
  auto passes = registerTlasGpuRebuildPasses(renderGraph, manager, 0u, "SoftwareBVHTests.tlasGpuLeafBeforeInternal");
  runGraphWithSortTimers(renderGraph, passes);

  const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), summary.nodeCount);
  const GenericCwbvhReadback gpuCwbvh = readGenericTlasGpuCwbvhReadback(renderGraph, manager, static_cast<uint32_t>(instances.size()), "GPU TLAS leaf-before-internal");
  validateTlasCanonicalHierarchy(canonicalNodes, summary.rootNodeIndex, instances, "GPU TLAS leaf-before-internal canonical hierarchy");
  validatePackedInternalChildLinksWithoutNodeRefs(gpuCwbvh.nodes, "GPU TLAS leaf-before-internal packed child links");
  validateTlasPackedLeafCoverageWithoutNodeRefs(gpuCwbvh.nodes, gpuCwbvh.leafInstanceIndices, instances, "GPU TLAS leaf-before-internal packed leaf coverage");
  expectTrue(
      std::any_of(
          gpuCwbvh.nodes.begin(),
          gpuCwbvh.nodes.end(),
          [](const TLASManager::CWBVHNodeGPU &node)
          {
            return packedNodeHasLeafBeforeInternalChild(node);
          }),
      "GPU TLAS leaf-before-internal fixture should contain a packed node with leaf slots before internal slots");

  deletePasses(passes);
}

void testTlasGpuRebuildAndRefitPasses(rendering::RHI &rhi)
{
  {
    rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
    TLASManager manager(&renderGraph, tlasTestSettings());
    const auto localBounds = meshBounds(makeUnitCubeMesh());

    std::vector<TLASManager::InstanceBuildInput> instances(4u);
    const std::array<math::Vec3f, 4u> translations = {
      math::Vec3f(-4.0f, 0.0f, -5.0f),
      math::Vec3f(4.0f, 0.0f, -5.0f),
      math::Vec3f(0.0f, -3.0f, -5.0f),
      math::Vec3f(0.0f, 3.0f, -5.0f),
    };
    for (uint32_t i = 0u; i < static_cast<uint32_t>(instances.size()); ++i)
    {
      instances[i].instanceId = 100u + i;
      instances[i].blasHandle = 3u;
      instances[i].transform = makeTranslation(translations[i]);
      instances[i].previousTransform = instances[i].transform;
      instances[i].worldBounds = translateBounds(localBounds, translations[i]);
      instances[i].sourceName = "cube" + std::to_string(i);
      instances[i].sourcePath = "tests/cube" + std::to_string(i);
    }

    const TLASManager::BuildSummary summary = manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
    auto passes = registerTlasGpuRebuildPasses(renderGraph, manager, 0u, "SoftwareBVHTests.tlasGpuBuild");
    runGraphWithSortTimers(renderGraph, passes);

    const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), summary.nodeCount);
    virtualgeometry::AABB expectedRoot = instances[0u].worldBounds;
    for (uint32_t i = 1u; i < static_cast<uint32_t>(instances.size()); ++i)
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        expectedRoot.minPoint[axis] = std::min(expectedRoot.minPoint[axis], instances[i].worldBounds.minPoint[axis]);
        expectedRoot.maxPoint[axis] = std::max(expectedRoot.maxPoint[axis], instances[i].worldBounds.maxPoint[axis]);
      }
    }

    virtualgeometry::AABB actualRoot{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      actualRoot.minPoint[axis] = canonicalNodes[summary.rootNodeIndex].minPoint[axis];
      actualRoot.maxPoint[axis] = canonicalNodes[summary.rootNodeIndex].maxPoint[axis];
    }
    compareBounds(actualRoot, expectedRoot, "GPU TLAS rebuild root bounds");

    const GenericCwbvhReadback gpuCwbvh = readGenericTlasGpuCwbvhReadback(renderGraph, manager, static_cast<uint32_t>(instances.size()), "GPU TLAS rebuild");
    std::set<uint32_t> observedLeafInstances(gpuCwbvh.leafInstanceIndices.begin(), gpuCwbvh.leafInstanceIndices.end());
    expectTrue(observedLeafInstances == std::set<uint32_t>{0u, 1u, 2u, 3u}, "GPU TLAS rebuild leaf remap should cover all instances");

    validateTlasPackedLeafCoverageWithoutNodeRefs(gpuCwbvh.nodes, gpuCwbvh.leafInstanceIndices, instances, "GPU TLAS rebuild packed leaf coverage");
    const auto leafSlots = collectPackedLeafSlots(gpuCwbvh.nodes.front(), gpuCwbvh.leafInstanceIndices);
    expectEqual(static_cast<uint32_t>(leafSlots.size()), static_cast<uint32_t>(instances.size()), "GPU TLAS root should expose all test instance leaves");
    for (const auto &[childSlot, instanceIndex] : leafSlots)
    {
      const virtualgeometry::AABB decodedLeaf = decodePackedChildBounds(gpuCwbvh.nodes.front(), childSlot);
      const virtualgeometry::AABB expectedLeaf = instances[instanceIndex].worldBounds;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        expectTrue(decodedLeaf.minPoint[axis] <= expectedLeaf.minPoint[axis] + kEpsilon, "GPU TLAS child min should enclose instance bounds");
        expectTrue(decodedLeaf.maxPoint[axis] >= expectedLeaf.maxPoint[axis] - kEpsilon, "GPU TLAS child max should enclose instance bounds");
      }
    }

    deletePasses(passes);
  }

  {
    rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
    TLASManager manager(&renderGraph, tlasTestSettings());
    const auto localBounds = meshBounds(makeUnitCubeMesh());

    std::vector<TLASManager::InstanceBuildInput> instances(12u);
    for (uint32_t i = 0u; i < static_cast<uint32_t>(instances.size()); ++i)
    {
      const math::Vec3f translation(static_cast<float>(i % 4u) * 9.0f - 16.0f, static_cast<float>(i / 4u) * 5.0f - 6.0f, -24.0f + static_cast<float>(i % 3u) * 3.0f);
      instances[i].instanceId = 300u + i;
      instances[i].blasHandle = 3u;
      instances[i].transform = makeTranslation(translation);
      instances[i].previousTransform = instances[i].transform;
      instances[i].worldBounds = translateBounds(localBounds, translation);
      instances[i].sourceName = "gpuManyCube" + std::to_string(i);
      instances[i].sourcePath = "tests/gpuManyCube" + std::to_string(i);
    }

    const TLASManager::BuildSummary summary = manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
    auto passes = registerTlasGpuRebuildPasses(renderGraph, manager, 0u, "SoftwareBVHTests.tlasGpuBuildMany");
    runGraphWithSortTimers(renderGraph, passes);

    const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), summary.nodeCount);
    const GenericCwbvhReadback gpuCwbvh = readGenericTlasGpuCwbvhReadback(renderGraph, manager, static_cast<uint32_t>(instances.size()), "GPU TLAS many-instance");
    virtualgeometry::AABB expectedRoot = instances[0u].worldBounds;
    for (uint32_t i = 1u; i < static_cast<uint32_t>(instances.size()); ++i)
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        expectedRoot.minPoint[axis] = std::min(expectedRoot.minPoint[axis], instances[i].worldBounds.minPoint[axis]);
        expectedRoot.maxPoint[axis] = std::max(expectedRoot.maxPoint[axis], instances[i].worldBounds.maxPoint[axis]);
      }
    }
    compareBounds(tlasCanonicalBounds(canonicalNodes[summary.rootNodeIndex]), expectedRoot, "GPU TLAS many-instance root bounds");
    validateTlasPackedLeafCoverageWithoutNodeRefs(gpuCwbvh.nodes, gpuCwbvh.leafInstanceIndices, instances, "GPU TLAS many-instance packed leaf coverage");

    deletePasses(passes);
  }

  {
    rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
    TLASManager manager(&renderGraph, tlasTestSettings());
    const auto localBounds = meshBounds(makeUnitCubeMesh());

    std::vector<TLASManager::InstanceBuildInput> instances(2u);
    instances[0u].instanceId = 10u;
    instances[0u].blasHandle = 3u;
    instances[0u].transform = makeTranslation(math::Vec3f(-2.0f, 0.0f, -5.0f));
    instances[0u].previousTransform = instances[0u].transform;
    instances[0u].worldBounds = translateBounds(localBounds, math::Vec3f(-2.0f, 0.0f, -5.0f));
    instances[0u].sourceName = "cubeA";
    instances[0u].sourcePath = "tests/cubeA";

    instances[1u].instanceId = 11u;
    instances[1u].blasHandle = 3u;
    instances[1u].transform = makeTranslation(math::Vec3f(2.0f, 0.0f, -5.0f));
    instances[1u].previousTransform = instances[1u].transform;
    instances[1u].worldBounds = translateBounds(localBounds, math::Vec3f(2.0f, 0.0f, -5.0f));
    instances[1u].sourceName = "cubeB";
    instances[1u].sourcePath = "tests/cubeB";
    manager.rebuild(instances, 2u);

    instances[1u].previousTransform = instances[1u].transform;
    instances[1u].transform = makeTranslation(math::Vec3f(6.0f, 0.0f, -5.0f));
    instances[1u].worldBounds = translateBounds(localBounds, math::Vec3f(6.0f, 0.0f, -5.0f));
    const TLASManager::BuildSummary refitSummary = manager.refit(instances, {1u});

    auto passes = registerTlasGpuRefitPasses(renderGraph, manager, 0u, "SoftwareBVHTests.tlasGpuRefit");
    runGraph(renderGraph);

    const auto canonicalNodes = readBufferVector<TLASManager::CanonicalNodeGPU>(renderGraph, manager.getCanonicalNodesBuffer(), refitSummary.nodeCount);
    validateTlasCanonicalHierarchy(canonicalNodes, refitSummary.rootNodeIndex, instances, "GPU TLAS refit canonical hierarchy");
    const auto gpuCwbvh = readBufferVector<TLASManager::CWBVHNodeGPU>(renderGraph, manager.getCwbvhNodesBuffer(), manager.getCwbvhNodeCount());
    const auto leafInstanceIndices = readBufferVector<uint32_t>(renderGraph, manager.getLeafInstanceIndicesBuffer(), manager.getLeafInstanceCount());
    validatePackedInternalChildLinksWithoutNodeRefs(gpuCwbvh, "GPU TLAS refit packed child links");
    validateTlasPackedLeafCoverageWithoutNodeRefs(gpuCwbvh, leafInstanceIndices, instances, "GPU TLAS refit packed leaf coverage");
    const auto leafSlots = collectPackedLeafSlots(gpuCwbvh.front(), leafInstanceIndices);
    expectEqual(static_cast<uint32_t>(leafSlots.size()), 2u, "GPU TLAS refit root should expose both leaves");
    for (const auto &[childSlot, instanceIndex] : leafSlots)
    {
      const virtualgeometry::AABB decodedLeaf = decodePackedChildBounds(gpuCwbvh.front(), childSlot);
      const virtualgeometry::AABB expectedLeaf = instances[instanceIndex].worldBounds;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        expectTrue(decodedLeaf.minPoint[axis] <= expectedLeaf.minPoint[axis] + kEpsilon, "GPU TLAS refit child min should enclose instance bounds");
        expectTrue(decodedLeaf.maxPoint[axis] >= expectedLeaf.maxPoint[axis] - kEpsilon, "GPU TLAS refit child max should enclose instance bounds");
      }
    }

    deletePasses(passes);
  }
}

struct SWRTRayTraceResult
{
  SWRTPass::StatsGPU stats{};
  SWRTPass::HitOutputGPU hit{};
};

struct SWRTExternalRayTraceResult
{
  SWRTPass::StatsGPU stats{};
  std::vector<SWRTPass::HitOutputGPU> hits;
};

struct RBVHTraceKernelRay
{
  math::Vec3f origin = math::Vec3f(0.0f, 0.0f, 0.0f);
  math::Vec3f direction = math::Vec3f(0.0f, 0.0f, -1.0f);
  math::Vec3f cardPoint = math::Vec3f(0.0f, 0.0f, 0.0f);
  uint32_t cardIndex = 0u;
};

float decodeRbvhCardDepth(const softwarebvh::RBVHBuildResult &build, const softwarebvh::RBVHCardPrimitive &card, const uint32_t atlasX, const uint32_t atlasY)
{
  const uint32_t pixelIndex = atlasY * build.atlasSize + atlasX;
  expectTrue(pixelIndex < build.depthAtlasR16.size(), "RBVH test depth pixel must be in range");
  const float depth01 = static_cast<float>(build.depthAtlasR16[pixelIndex]) / 65535.0f;
  return card.depthMin + depth01 * (card.depthMax - card.depthMin);
}

math::Vec3f reconstructRbvhCardPoint(const softwarebvh::RBVHBuildResult &build, const uint32_t cardIndex, const uint32_t atlasX, const uint32_t atlasY)
{
  const softwarebvh::RBVHCardPrimitive &card = build.cards[cardIndex];
  const float localU = ((static_cast<float>(atlasX - card.atlasX) + 0.5f) / static_cast<float>(card.atlasWidth) * 2.0f - 1.0f) * card.halfExtentU;
  const float localV = ((static_cast<float>(atlasY - card.atlasY) + 0.5f) / static_cast<float>(card.atlasHeight) * 2.0f - 1.0f) * card.halfExtentV;
  const float absoluteDepth = decodeRbvhCardDepth(build, card, atlasX, atlasY);
  const math::Vec3f origin = loadArray3(card.origin);
  const math::Vec3f axisN = loadArray3(card.axisN);
  const float originDepth = origin.dot(axisN);
  return origin + loadArray3(card.axisU) * localU + loadArray3(card.axisV) * localV + axisN * (absoluteDepth - originDepth);
}

std::vector<RBVHTraceKernelRay> makeRandomRbvhBunnyRays(const softwarebvh::RBVHBuildResult &build, const virtualgeometry::FallbackMeshData &mesh, const uint32_t rayCount)
{
  std::vector<RBVHTraceKernelRay> rays;
  rays.reserve(rayCount);
  const math::Vec3f boundsMin(mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2]);
  const math::Vec3f boundsMax(mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]);
  const float meshDiagonal = std::max((boundsMax - boundsMin).length(), 1.0f);
  uint32_t rng = 0x6d2b79f5u;
  auto nextRandom = [&]()
  {
    rng = rng * 1664525u + 1013904223u;
    return rng;
  };

  for (uint32_t attempt = 0u; attempt < 20000u && rays.size() < rayCount; ++attempt)
  {
    if (build.cards.empty())
      break;

    const uint32_t cardIndex = nextRandom() % static_cast<uint32_t>(build.cards.size());
    const softwarebvh::RBVHCardPrimitive &card = build.cards[cardIndex];
    if (card.atlasWidth == 0u || card.atlasHeight == 0u)
      continue;

    const uint32_t localX = nextRandom() % card.atlasWidth;
    const uint32_t localY = nextRandom() % card.atlasHeight;
    const uint32_t atlasX = card.atlasX + localX;
    const uint32_t atlasY = card.atlasY + localY;
    const uint32_t pixelIndex = atlasY * build.atlasSize + atlasX;
    if (pixelIndex >= build.coverageAtlasR8.size() || build.coverageAtlasR8[pixelIndex] == 0u)
      continue;

    const math::Vec3f point = reconstructRbvhCardPoint(build, cardIndex, atlasX, atlasY);
    const math::Vec3f normal = loadArray3(card.axisN).normalize();
    const float rayDistance = std::max(meshDiagonal * 0.35f, std::max(card.samplingError * 32.0f, 0.01f));
    rays.push_back(
        RBVHTraceKernelRay{
          .origin = point + normal * rayDistance,
          .direction = (point - (point + normal * rayDistance)).normalize(),
          .cardPoint = point,
          .cardIndex = cardIndex,
        });
  }

  return rays;
}

bool findNearestTriangleHitT(const virtualgeometry::FallbackMeshData &mesh, const math::Vec3f &rayOrigin, const math::Vec3f &rayDirection, float *outT)
{
  float bestT = std::numeric_limits<float>::max();
  bool hit = false;
  const uint32_t triangleCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  for (uint32_t primitiveIndex = 0u; primitiveIndex < triangleCount; ++primitiveIndex)
  {
    const uint32_t baseIndex = primitiveIndex * 3u;
    const math::Vec3f a = loadPosition(mesh.vertices[mesh.indices[baseIndex + 0u]]);
    const math::Vec3f b = loadPosition(mesh.vertices[mesh.indices[baseIndex + 1u]]);
    const math::Vec3f c = loadPosition(mesh.vertices[mesh.indices[baseIndex + 2u]]);
    float candidateT = 0.0f;
    if (intersectTriangleRay(rayOrigin, rayDirection, a, b, c, &candidateT) && candidateT > 0.001f && candidateT < bestT)
    {
      bestT = candidateT;
      hit = true;
    }
  }

  if (hit && outT != nullptr)
    *outT = bestT;
  return hit;
}

SWRTRayTraceResult runSingleRayTraceForMesh(
    rendering::RHI &rhi,
    const BLASManager::BuildInput &blasInput,
    const math::Vec3f &cameraPosition,
    const math::Vec3f &cameraForward,
    const math::Vec3f &instanceTranslation,
    bool buildGpuCwbvh = false,
    bool enableDynamicRayFetch = false,
    SWRTPass::PrimitiveMode primitiveMode = SWRTPass::PrimitiveMode::Triangle,
    SWRTPass::TLASMode tlasMode = SWRTPass::TLASMode::CWBVH)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager blasManager(&renderGraph, blasTestSettingsForMesh(blasInput.mesh, blasInput.prebuilt ? &*blasInput.prebuilt : nullptr, blasInput.rbvhBuild ? &*blasInput.rbvhBuild : nullptr));
  TLASManager tlasManager(&renderGraph, tlasTestSettings());
  blasManager.syncFromCPU({blasInput});

  const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInput.sourcePath);
  expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, "Ray trace BLAS handle should be valid");

  virtualgeometry::AABB localBounds{};
  expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), "Ray trace local bounds lookup should succeed");

  TLASManager::InstanceBuildInput instance{};
  instance.instanceId = 42u;
  instance.blasHandle = blasHandle;
  instance.transform = makeTranslation(instanceTranslation);
  instance.previousTransform = instance.transform;
  instance.worldBounds = translateBounds(localBounds, instanceTranslation);
  instance.sourceName = "raytrace_triangle";
  instance.sourcePath = "tests/raytrace_triangle";
  tlasManager.rebuild({instance}, 1u);

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = 1u;
  settings.height = 1u;
  settings.cameraPosition = cameraPosition;
  settings.cameraForward = cameraForward;
  makeCameraBasis(cameraForward, &settings.cameraRight, &settings.cameraUp);
  settings.verticalFovRadians = math::radians(60.0f);
  settings.tMin = 0.001f;
  settings.tMax = 100.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.enableDynamicRayFetch = enableDynamicRayFetch;
  settings.primitiveMode = primitiveMode;
  settings.tlasMode = tlasMode;
  softwarebvh::rendering::SoftwareBLASGPUPreparePass *preparePass = nullptr;
  softwarebvh::rendering::SoftwareBLASGPUFinalizePass *finalizePass = nullptr;
  uint32_t tracePassIndex = 0u;
  if (buildGpuCwbvh)
  {
    preparePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUPreparePass>("SoftwareBVHTests.prepareTrace", 0u, blasManager, blasHandle);
    finalizePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUFinalizePass>("SoftwareBVHTests.finalizeTrace", 1u, blasManager, blasHandle);
    tracePassIndex = 2u;
  }
  auto *tracePass = renderGraph.registerPass<SWRTPass>("SoftwareBVHTests.raytrace", tracePassIndex, tlasManager, blasManager, settings);
  expectTrue(tracePass->getStatsBuffer().isValid(), "SWRT stats buffer should be valid immediately after pass registration");
  expectTrue(tracePass->getHitOutputBuffer().isValid(), "SWRT hit output buffer should be valid immediately after pass registration");
  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.raytrace.hitOutputReadback",
        .size = sizeof(SWRTPass::HitOutputGPU),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(
      "SoftwareBVHTests.raytrace.hitOutputCopy", tracePassIndex + 1u, tracePass->getHitOutputBuffer(), 0u, sizeof(SWRTPass::HitOutputGPU), hitOutputReadback, 0u, sizeof(SWRTPass::HitOutputGPU));

  runGraph(renderGraph);
  SWRTRayTraceResult result{};
  result.stats = readBufferValue<SWRTPass::StatsGPU>(renderGraph, tracePass->getStatsBuffer());
  result.hit = readBufferValue<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback);
  delete hitOutputCopyPass;
  delete tracePass;
  delete finalizePass;
  delete preparePass;
  return result;
}

SWRTExternalRayTraceResult
runExternalRayTraceForMesh(
    rendering::RHI &rhi,
    const BLASManager::BuildInput &blasInput,
    const std::vector<SWRTPass::TraceRayGPU> &rays,
    const math::Vec3f &instanceTranslation,
    const bool enableRaySorting,
    const bool enableRayStreamTraversal = false,
    const uint32_t streamMaxDispatchPasses = 12u,
    const uint32_t streamTaskBudgetMultiplier = 8u,
    const uint32_t streamMaxLocalNodeIterations = 8u)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager blasManager(&renderGraph, blasTestSettingsForMesh(blasInput.mesh, blasInput.prebuilt ? &*blasInput.prebuilt : nullptr));
  TLASManager tlasManager(&renderGraph, tlasTestSettings());
  blasManager.syncFromCPU({blasInput});

  const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInput.sourcePath);
  expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, "External SWRT BLAS handle should be valid");

  virtualgeometry::AABB localBounds{};
  expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), "External SWRT local bounds lookup should succeed");

  TLASManager::InstanceBuildInput instance{};
  instance.instanceId = 42u;
  instance.blasHandle = blasHandle;
  instance.transform = makeTranslation(instanceTranslation);
  instance.previousTransform = instance.transform;
  instance.worldBounds = translateBounds(localBounds, instanceTranslation);
  instance.sourceName = "external_raytrace_triangle";
  instance.sourcePath = "tests/external_raytrace_triangle";
  tlasManager.rebuild({instance}, 1u);

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = std::max<uint32_t>(static_cast<uint32_t>(rays.size()), 1u);
  settings.height = 1u;
  settings.generateCameraRays = false;
  settings.rayCount = static_cast<uint32_t>(rays.size());
  settings.tMin = 0.001f;
  settings.tMax = 100.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.primitiveMode = SWRTPass::PrimitiveMode::Triangle;
  settings.enableRaySorting = enableRaySorting;
  settings.enableRayStreamTraversal = enableRayStreamTraversal;
  settings.streamMaxDispatchPasses = streamMaxDispatchPasses;
  settings.streamTaskBudgetMultiplier = streamTaskBudgetMultiplier;
  settings.streamMaxLocalNodeIterations = streamMaxLocalNodeIterations;
  settings.shaderVariant = enableRayStreamTraversal ? SWRTPass::ShaderVariant::Debug : SWRTPass::ShaderVariant::Release;

  auto *tracePass = renderGraph.registerPass<SWRTPass>("SoftwareBVHTests.externalRaytrace", 1u, tlasManager, blasManager, settings);
  const uint64_t rayBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::TraceRayGPU);
  const rendering::Buffer rayUpload = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.externalRaytrace.rayUpload",
        .size = std::max<uint64_t>(rayBytes, 4u),
        .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  renderGraph.bufferWrite(rayUpload, 0u, rayBytes, const_cast<SWRTPass::TraceRayGPU *>(rays.data()));
  auto *rayUploadPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests.externalRaytrace.rayUploadCopy", 0u, rayUpload, 0u, rayBytes, tracePass->getRayInputBuffer(), 0u, rayBytes);

  const uint64_t hitBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::HitOutputGPU);
  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.externalRaytrace.hitOutputReadback",
        .size = std::max<uint64_t>(hitBytes, 4u),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass =
      renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests.externalRaytrace.hitOutputCopy", 2u, tracePass->getHitOutputBuffer(), 0u, hitBytes, hitOutputReadback, 0u, hitBytes);

  runGraph(renderGraph);
  SWRTExternalRayTraceResult result{};
  result.stats = readBufferValue<SWRTPass::StatsGPU>(renderGraph, tracePass->getStatsBuffer());
  result.hits = readBufferVector<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback, static_cast<uint32_t>(rays.size()));

  delete hitOutputCopyPass;
  delete rayUploadPass;
  delete tracePass;
  return result;
}

SWRTPass::TraceRayGPU makeTraceRay(const math::Vec3f &origin, const math::Vec3f &direction, float tMax);

SWRTExternalRayTraceResult runMultiInstanceStreamHashTrace(rendering::RHI &rhi, const bool enableRayStreamTraversal)
{
  constexpr uint32_t kInstanceCount = 96u;
  constexpr uint32_t kGridWidth = 12u;
  constexpr float kSpacing = 2.5f;

  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/stream_multi_instance_hash_triangle.mesh";
  blasInput.fallbackPath = "tests/stream_multi_instance_hash_triangle.fallback.mesh";
  blasInput.mesh = makeFrontFacingTriangleMesh(0.0f);

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager blasManager(&renderGraph, blasTestSettingsForMesh(blasInput.mesh));
  blasManager.syncFromCPU({blasInput});

  const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInput.sourcePath);
  expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, "Multi-instance stream BLAS handle should be valid");

  virtualgeometry::AABB localBounds{};
  expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), "Multi-instance stream local bounds lookup should succeed");

  std::vector<TLASManager::InstanceBuildInput> instances;
  std::vector<SWRTPass::TraceRayGPU> rays;
  instances.reserve(kInstanceCount);
  rays.reserve(kInstanceCount);
  for (uint32_t instanceIndex = 0u; instanceIndex < kInstanceCount; ++instanceIndex)
  {
    const uint32_t x = instanceIndex % kGridWidth;
    const uint32_t y = instanceIndex / kGridWidth;
    const math::Vec3f translation(
        (static_cast<float>(x) - static_cast<float>(kGridWidth) * 0.5f) * kSpacing,
        static_cast<float>(y) * kSpacing,
        -5.0f);

    TLASManager::InstanceBuildInput instance{};
    instance.instanceId = 300000u + instanceIndex;
    instance.blasHandle = blasHandle;
    instance.transform = makeTranslation(translation);
    instance.previousTransform = instance.transform;
    instance.worldBounds = translateBounds(localBounds, translation);
    instance.sourceName = "stream_multi_instance_" + std::to_string(instanceIndex);
    instance.sourcePath = blasInput.sourcePath;
    instances.push_back(instance);

    rays.push_back(makeTraceRay(translation + math::Vec3f(0.0f, -0.15f, 2.0f), math::Vec3f(0.0f, 0.0f, -1.0f), 16.0f));
  }

  TLASManager::Settings tlasSettings = tlasTestSettings();
  tlasSettings.maxInstances = kInstanceCount;
  TLASManager tlasManager(&renderGraph, tlasSettings);
  tlasManager.rebuild(instances, static_cast<uint32_t>(instances.size()));

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = static_cast<uint32_t>(rays.size());
  settings.height = 1u;
  settings.generateCameraRays = false;
  settings.rayCount = static_cast<uint32_t>(rays.size());
  settings.tMin = 0.001f;
  settings.tMax = 32.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.primitiveMode = SWRTPass::PrimitiveMode::Triangle;
  settings.tlasMode = SWRTPass::TLASMode::CWBVH;
  settings.enableRaySorting = false;
  settings.enableRayStreamTraversal = enableRayStreamTraversal;
  settings.streamMaxDispatchPasses = 32u;
  settings.streamTaskBudgetMultiplier = 256u;
  settings.streamMaxLocalNodeIterations = 1u;
  settings.shaderVariant = enableRayStreamTraversal ? SWRTPass::ShaderVariant::Debug : SWRTPass::ShaderVariant::Release;

  auto *tracePass = renderGraph.registerPass<SWRTPass>(
      enableRayStreamTraversal ? "SoftwareBVHTests.streamMultiInstanceHash" : "SoftwareBVHTests.localMultiInstanceHash", 1u, tlasManager, blasManager, settings);

  const uint64_t rayBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::TraceRayGPU);
  const rendering::Buffer rayUpload = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.streamMultiInstanceHash.rayUpload",
        .size = std::max<uint64_t>(rayBytes, 4u),
        .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  renderGraph.bufferWrite(rayUpload, 0u, rayBytes, const_cast<SWRTPass::TraceRayGPU *>(rays.data()));
  auto *rayUploadPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(
      "SoftwareBVHTests.streamMultiInstanceHash.rayUploadCopy", 0u, rayUpload, 0u, rayBytes, tracePass->getRayInputBuffer(), 0u, rayBytes);

  const uint64_t hitBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::HitOutputGPU);
  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.streamMultiInstanceHash.hitOutputReadback",
        .size = std::max<uint64_t>(hitBytes, 4u),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(
      "SoftwareBVHTests.streamMultiInstanceHash.hitOutputCopy", 2u, tracePass->getHitOutputBuffer(), 0u, hitBytes, hitOutputReadback, 0u, hitBytes);

  runGraph(renderGraph);
  SWRTExternalRayTraceResult result{};
  result.stats = readBufferValue<SWRTPass::StatsGPU>(renderGraph, tracePass->getStatsBuffer());
  result.hits = readBufferVector<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback, static_cast<uint32_t>(rays.size()));

  delete hitOutputCopyPass;
  delete rayUploadPass;
  delete tracePass;
  return result;
}

SWRTPass::TraceRayGPU makeTraceRay(const math::Vec3f &origin, const math::Vec3f &direction, const float tMax = 100.0f)
{
  SWRTPass::TraceRayGPU ray{};
  ray.originTMin[0] = origin[0];
  ray.originTMin[1] = origin[1];
  ray.originTMin[2] = origin[2];
  ray.originTMin[3] = 0.001f;
  ray.directionTMax[0] = direction[0];
  ray.directionTMax[1] = direction[1];
  ray.directionTMax[2] = direction[2];
  ray.directionTMax[3] = tMax;
  return ray;
}

void expectTraceHitMatches(const SWRTPass::HitOutputGPU &actual, const SWRTPass::HitOutputGPU &expected, const std::string &label)
{
  expectEqual(actual.instanceId, expected.instanceId, label + " instance id");
  expectEqual(actual.triangleId, expected.triangleId, label + " triangle id");
  expectEqual(actual.tlasInstanceIndex, expected.tlasInstanceIndex, label + " TLAS instance index");
  if (expected.instanceId != UINT32_MAX)
  {
    expectNear(actual.hitT, expected.hitT, label + " hit t", 1.0e-3f);
    expectNear(actual.baryU, expected.baryU, label + " bary u", 1.0e-3f);
    expectNear(actual.baryV, expected.baryV, label + " bary v", 1.0e-3f);
  }
}

void expectTraceResultsMatch(const SWRTExternalRayTraceResult &streamed, const SWRTExternalRayTraceResult &baseline, const std::string &label, const bool expectLegacyQueueWork = false)
{
  expectEqual(static_cast<uint32_t>(streamed.hits.size()), static_cast<uint32_t>(baseline.hits.size()), label + " hit count");
  for (uint32_t rayIndex = 0u; rayIndex < static_cast<uint32_t>(baseline.hits.size()); ++rayIndex)
    expectTraceHitMatches(streamed.hits[rayIndex], baseline.hits[rayIndex], label + " ray[" + std::to_string(rayIndex) + "]");
  if (expectLegacyQueueWork)
  {
    expectTrue(streamed.stats.workgroupQueuePushes > 0u, label + " stream path should push packet tasks");
    expectTrue(streamed.stats.workgroupQueuePops > 0u, label + " stream path should consume packet tasks");
  }
}

uint32_t countTraceHits(const std::vector<SWRTPass::HitOutputGPU> &hits)
{
  uint32_t count = 0u;
  for (const SWRTPass::HitOutputGPU &hit : hits)
  {
    if (hit.instanceId != UINT32_MAX && hit.triangleId != UINT32_MAX)
      ++count;
  }
  return count;
}

SWRTRayTraceResult runSingleTriangleRayTrace(rendering::RHI &rhi, const math::Vec3f &cameraForward, const math::Vec3f &instanceTranslation, SWRTPass::TLASMode tlasMode = SWRTPass::TLASMode::CWBVH)
{
  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/raytrace_triangle.mesh";
  blasInput.fallbackPath = "tests/raytrace_triangle.fallback.mesh";
  blasInput.mesh = makeFrontFacingTriangleMesh(0.0f);
  return runSingleRayTraceForMesh(rhi, blasInput, math::Vec3f(0.0f, 0.0f, 0.0f), cameraForward, instanceTranslation, false, false, SWRTPass::PrimitiveMode::Triangle, tlasMode);
}

SWRTRayTraceResult runMultiInstanceRayTrace(
    rendering::RHI &rhi,
    const std::vector<math::Vec3f> &instanceTranslations,
    const math::Vec3f &cameraPosition,
    const math::Vec3f &cameraForward,
    SWRTPass::TLASMode tlasMode = SWRTPass::TLASMode::CWBVH)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/raytrace_triangle_multi.mesh";
  blasInput.fallbackPath = "tests/raytrace_triangle_multi.fallback.mesh";
  blasInput.mesh = makeFrontFacingTriangleMesh(0.0f);

  BLASManager blasManager(&renderGraph, blasTestSettingsForMesh(blasInput.mesh, nullptr));
  TLASManager tlasManager(&renderGraph, tlasTestSettings());
  blasManager.syncFromCPU({blasInput});

  const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInput.sourcePath);
  expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, "Multi-instance SWRT BLAS handle should be valid");

  virtualgeometry::AABB localBounds{};
  expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), "Multi-instance SWRT local bounds lookup should succeed");

  std::vector<TLASManager::InstanceBuildInput> instances;
  instances.reserve(instanceTranslations.size());
  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(instanceTranslations.size()); ++instanceIndex)
  {
    TLASManager::InstanceBuildInput instance{};
    instance.instanceId = 100u + instanceIndex;
    instance.blasHandle = blasHandle;
    instance.transform = makeTranslation(instanceTranslations[instanceIndex]);
    instance.previousTransform = instance.transform;
    instance.worldBounds = translateBounds(localBounds, instanceTranslations[instanceIndex]);
    instance.sourceName = "raytrace_triangle_multi";
    instance.sourcePath = "tests/raytrace_triangle_multi";
    instances.push_back(instance);
  }
  tlasManager.rebuild(instances, static_cast<uint32_t>(instances.size()));

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = 1u;
  settings.height = 1u;
  settings.cameraPosition = cameraPosition;
  settings.cameraForward = cameraForward;
  makeCameraBasis(cameraForward, &settings.cameraRight, &settings.cameraUp);
  settings.verticalFovRadians = math::radians(60.0f);
  settings.tMin = 0.001f;
  settings.tMax = 100.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.tlasMode = tlasMode;

  auto *tracePass = renderGraph.registerPass<SWRTPass>("SoftwareBVHTests.multiInstanceRaytrace", 0u, tlasManager, blasManager, settings);
  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.multiInstanceRaytrace.hitOutputReadback",
        .size = sizeof(SWRTPass::HitOutputGPU),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(
      "SoftwareBVHTests.multiInstanceRaytrace.hitOutputCopy", 1u, tracePass->getHitOutputBuffer(), 0u, sizeof(SWRTPass::HitOutputGPU), hitOutputReadback, 0u, sizeof(SWRTPass::HitOutputGPU));
  runGraph(renderGraph);
  SWRTRayTraceResult result{};
  result.stats = readBufferValue<SWRTPass::StatsGPU>(renderGraph, tracePass->getStatsBuffer());
  result.hit = readBufferValue<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback);
  delete hitOutputCopyPass;
  delete tracePass;
  return result;
}

SWRTRayTraceResult runWorldCwbvhTriangleRayTrace(
    rendering::RHI &rhi,
    const std::vector<math::Vec3f> &instanceTranslations,
    const math::Vec3f &cameraPosition,
    const math::Vec3f &cameraForward,
    const virtualgeometry::FallbackMeshData *meshOverride = nullptr)
{
  using Scene = softwarebvh::SoftwareRayTraceScene;
  using WorldBLASPass = softwarebvh::rendering::SoftwareWorldBLASUpdatePass;

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/world_cwbvh_trace_triangle.mesh";
  blasInput.fallbackPath = "tests/world_cwbvh_trace_triangle.fallback.mesh";
  blasInput.mesh = meshOverride != nullptr ? *meshOverride : makeFrontFacingTriangleMesh(0.0f);

  BLASManager blasManager(&renderGraph, blasTestSettingsForMesh(blasInput.mesh, nullptr));
  TLASManager tlasManager(&renderGraph, tlasTestSettings());
  blasManager.syncFromCPU({blasInput});

  const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInput.sourcePath);
  expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, "World CWBVH trace BLAS handle should be valid");

  virtualgeometry::AABB localBounds{};
  expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), "World CWBVH trace local bounds lookup should succeed");

  std::vector<TLASManager::InstanceBuildInput> instances;
  instances.reserve(instanceTranslations.size());
  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(instanceTranslations.size()); ++instanceIndex)
  {
    TLASManager::InstanceBuildInput instance{};
    instance.instanceId = 100u + instanceIndex;
    instance.blasHandle = blasHandle;
    instance.flags = TLASManager::INSTANCE_FLAG_DYNAMIC;
    instance.transform = makeTranslation(instanceTranslations[instanceIndex]);
    instance.previousTransform = instance.transform;
    instance.worldBounds = translateBounds(localBounds, instanceTranslations[instanceIndex]);
    instance.sourceName = "world_cwbvh_trace_triangle";
    instance.sourcePath = "tests/world_cwbvh_trace_triangle";
    instances.push_back(instance);
  }
  tlasManager.rebuild(instances, static_cast<uint32_t>(instances.size()));

  const std::vector<Scene::SourceBLAS> worldSources = blasManager.buildRayTraceSceneSources();
  expectTrue(!worldSources.empty(), "World CWBVH trace should expose source BVH8 data");
  const std::vector<TLASManager::InstanceBuildInput> &tlasInputs = tlasManager.getInstanceInputs();
  std::vector<Scene::InstanceInput> worldInstances;
  std::vector<Scene::InstanceTransformGPU> worldInstanceTransforms;
  worldInstances.reserve(tlasInputs.size());
  worldInstanceTransforms.resize(tlasInputs.size());
  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(tlasInputs.size()); ++instanceIndex)
  {
    const auto &input = tlasInputs[instanceIndex];
    worldInstances.push_back(
        Scene::InstanceInput{
          .instanceIndex = instanceIndex,
          .sceneInstanceId = input.instanceId,
          .blasHandle = input.blasHandle,
          .flags = input.flags,
          .transformDirty = true,
        });
    const math::Mat4f inverseTransform = input.transform.inverse();
    std::memcpy(worldInstanceTransforms[instanceIndex].transform, input.transform.data, sizeof(worldInstanceTransforms[instanceIndex].transform));
    std::memcpy(worldInstanceTransforms[instanceIndex].inverseTransform, inverseTransform.data, sizeof(worldInstanceTransforms[instanceIndex].inverseTransform));
  }

  const uint32_t tlasCwbvhNodeCount = std::max(tlasManager.getCwbvhNodeCount(), 1u);
  Scene::Settings sceneSettings = Scene::estimateSettings(tlasCwbvhNodeCount, worldSources, worldInstances, 64u);
  Scene worldScene(sceneSettings);
  const Scene::Plan worldPlan = worldScene.buildPlan(tlasCwbvhNodeCount, worldSources, worldInstances);
  expectTrue(worldPlan.valid, "World CWBVH trace scene plan should be valid: " + worldPlan.error);
  expectTrue(!worldPlan.updateInstanceRangeIndices.empty(), "World CWBVH trace should queue world BLAS updates");

  std::vector<Scene::WorldInstanceRangeGPU> worldRanges = Scene::buildInstanceRangeGPU(worldPlan);
  std::vector<Scene::TriangleUpdateItemGPU> triangleItems = Scene::buildTriangleUpdateItems(worldPlan, worldSources);
  std::vector<Scene::BVH8UpdateItemGPU> bvh8Items = Scene::buildBVH8UpdateItems(worldPlan, worldSources);
  std::vector<Scene::LeafRemapUpdateItemGPU> leafItems = Scene::buildLeafRemapUpdateItems(worldPlan, worldSources);
  expectEqual(worldRanges[0u].sceneInstanceId, 100u, "World CWBVH CPU range should preserve scene instance id");
  expectEqual(worldRanges[0u].instanceIndex, 0u, "World CWBVH CPU range should preserve TLAS instance index");

  auto makeWorldBuffer = [&](const char *name, const uint64_t size, const rendering::BufferUsage extraUsage = rendering::BufferUsage::BufferUsage_None)
  {
    return renderGraph.createBuffer(
        rendering::BufferInfo{
          .name = name,
          .size = std::max<uint64_t>(size, 4u),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_CopyDst | extraUsage,
        });
  };
  auto makeUploadBuffer = [&](const char *name, const uint64_t size)
  {
    return renderGraph.createBuffer(
        rendering::BufferInfo{
          .name = name,
          .size = std::max<uint64_t>(size, 4u),
          .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
  };
  auto uploadToWorldBuffer = [&](const char *name, const rendering::Buffer &destination, const void *data, const uint64_t size)
  {
    const rendering::Buffer uploadBuffer = makeUploadBuffer(name, size);
    renderGraph.bufferWrite(uploadBuffer, 0u, size, const_cast<void *>(data));
    return renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(std::string(name) + ".copy", 0u, uploadBuffer, 0u, size, destination, 0u, size);
  };

  WorldBLASPass::Settings worldBlasSettings{};
  worldBlasSettings.instanceRangeCount = static_cast<uint32_t>(worldRanges.size());
  worldBlasSettings.triangleUpdateItemCount = static_cast<uint32_t>(triangleItems.size());
  worldBlasSettings.bvh8UpdateItemCount = static_cast<uint32_t>(bvh8Items.size());
  worldBlasSettings.leafRemapUpdateItemCount = static_cast<uint32_t>(leafItems.size());
  worldBlasSettings.maxInstanceRangeCount = std::max<uint32_t>(worldBlasSettings.instanceRangeCount, 1u);
  worldBlasSettings.maxUpdateInstanceCount = std::max<uint32_t>(static_cast<uint32_t>(worldPlan.updateInstanceRangeIndices.size()), 1u);
  worldBlasSettings.maxTriangleUpdateItemCount = std::max<uint32_t>(worldBlasSettings.triangleUpdateItemCount, 1u);
  worldBlasSettings.maxBVH8UpdateItemCount = std::max<uint32_t>(worldBlasSettings.bvh8UpdateItemCount, 1u);
  worldBlasSettings.maxLeafRemapUpdateItemCount = std::max<uint32_t>(worldBlasSettings.leafRemapUpdateItemCount, 1u);
  worldBlasSettings.maxInstanceTransformCount = std::max<uint32_t>(static_cast<uint32_t>(worldInstanceTransforms.size()), 1u);
  worldBlasSettings.maxSourceTriangleCount = std::max<uint32_t>(blasManager.getLastBuildSummary().primitiveCount, 1u);
  worldBlasSettings.maxWorldTriangleCount = std::max<uint32_t>(worldPlan.worldTriangleCount, 1u);
  worldBlasSettings.maxSourceBVH8NodeCount = std::max<uint32_t>(blasManager.getSourceBVH8NodeCount(), 1u);
  worldBlasSettings.maxWorldBVH8NodeCount = std::max<uint32_t>(worldPlan.worldBVH8NodeCount, 1u);
  worldBlasSettings.maxSourceLeafRemapCount = std::max<uint32_t>(static_cast<uint32_t>(blasManager.getGPULeafRemapBufferSize() / sizeof(uint32_t)), 1u);
  worldBlasSettings.maxWorldLeafRemapCount = std::max<uint32_t>(worldPlan.worldLeafRemapCount, 1u);
  worldBlasSettings.maxSceneCWBVHNodeCount = std::max<uint32_t>(worldPlan.sceneCWBVHNodeCount, tlasCwbvhNodeCount);
  worldBlasSettings.workgroupSize = 64u;
  worldBlasSettings.useIndirectDispatch = false;

  WorldBLASPass::Buffers worldBlasBuffers{};
  worldBlasBuffers.instanceRanges = makeWorldBuffer("SoftwareBVHTests.world.instanceRanges", static_cast<uint64_t>(worldBlasSettings.maxInstanceRangeCount) * sizeof(Scene::WorldInstanceRangeGPU));
  worldBlasBuffers.updateInstanceRangeIndices = makeWorldBuffer("SoftwareBVHTests.world.updateInstanceRanges", static_cast<uint64_t>(worldBlasSettings.maxUpdateInstanceCount) * sizeof(uint32_t));
  worldBlasBuffers.updateCounters = makeWorldBuffer("SoftwareBVHTests.world.updateCounters", sizeof(Scene::UpdateCountersGPU));
  worldBlasBuffers.dispatchArgs = makeWorldBuffer("SoftwareBVHTests.world.dispatchArgs", 3u * sizeof(Scene::DispatchIndirectArgsGPU), rendering::BufferUsage::BufferUsage_Indirect);
  worldBlasBuffers.triangleUpdateItems = makeWorldBuffer("SoftwareBVHTests.world.triangleItems", static_cast<uint64_t>(worldBlasSettings.maxTriangleUpdateItemCount) * sizeof(Scene::TriangleUpdateItemGPU));
  worldBlasBuffers.bvh8UpdateItems = makeWorldBuffer("SoftwareBVHTests.world.bvh8Items", static_cast<uint64_t>(worldBlasSettings.maxBVH8UpdateItemCount) * sizeof(Scene::BVH8UpdateItemGPU));
  worldBlasBuffers.leafRemapUpdateItems = makeWorldBuffer("SoftwareBVHTests.world.leafItems", static_cast<uint64_t>(worldBlasSettings.maxLeafRemapUpdateItemCount) * sizeof(Scene::LeafRemapUpdateItemGPU));
  worldBlasBuffers.instanceTransforms = makeWorldBuffer("SoftwareBVHTests.world.instanceTransforms", static_cast<uint64_t>(worldBlasSettings.maxInstanceTransformCount) * sizeof(Scene::InstanceTransformGPU));
  worldBlasBuffers.sourceTriangles = blasManager.getTriangleTransformsBuffer();
  worldBlasBuffers.worldTriangles = makeWorldBuffer("SoftwareBVHTests.world.triangles", static_cast<uint64_t>(worldBlasSettings.maxWorldTriangleCount) * sizeof(Scene::TriBaryTransformGPU));
  worldBlasBuffers.sourceBVH8Nodes = blasManager.getSourceBVH8NodesBuffer();
  worldBlasBuffers.worldBVH8Nodes = makeWorldBuffer("SoftwareBVHTests.world.bvh8Nodes", static_cast<uint64_t>(worldBlasSettings.maxWorldBVH8NodeCount) * sizeof(Scene::BVH8NodeGPU));
  worldBlasBuffers.sourceLeafRemap = blasManager.getGPULeafRemapBuffer();
  worldBlasBuffers.worldLeafRemap = makeWorldBuffer("SoftwareBVHTests.world.leafRemap", static_cast<uint64_t>(worldBlasSettings.maxWorldLeafRemapCount) * sizeof(Scene::WorldLeafEntryGPU));
  worldBlasBuffers.sceneCWBVHNodes = makeWorldBuffer("SoftwareBVHTests.world.sceneCwbvh", static_cast<uint64_t>(worldBlasSettings.maxSceneCWBVHNodeCount) * sizeof(Scene::PackedCWBVHNodeGPU));
  worldBlasBuffers.sourcePrimitiveRemap = blasManager.getPrimitiveRemapBuffer();

  auto *uploadRangesPass = uploadToWorldBuffer("SoftwareBVHTests.world.uploadInstanceRanges", worldBlasBuffers.instanceRanges, worldRanges.data(), worldRanges.size() * sizeof(Scene::WorldInstanceRangeGPU));
  auto *uploadTransformsPass =
      uploadToWorldBuffer("SoftwareBVHTests.world.uploadInstanceTransforms", worldBlasBuffers.instanceTransforms, worldInstanceTransforms.data(), worldInstanceTransforms.size() * sizeof(Scene::InstanceTransformGPU));
  auto *uploadTriangleItemsPass =
      uploadToWorldBuffer("SoftwareBVHTests.world.uploadTriangleItems", worldBlasBuffers.triangleUpdateItems, triangleItems.data(), triangleItems.size() * sizeof(Scene::TriangleUpdateItemGPU));
  auto *uploadBvh8ItemsPass = uploadToWorldBuffer("SoftwareBVHTests.world.uploadBvh8Items", worldBlasBuffers.bvh8UpdateItems, bvh8Items.data(), bvh8Items.size() * sizeof(Scene::BVH8UpdateItemGPU));
  auto *uploadLeafItemsPass = uploadToWorldBuffer("SoftwareBVHTests.world.uploadLeafItems", worldBlasBuffers.leafRemapUpdateItems, leafItems.data(), leafItems.size() * sizeof(Scene::LeafRemapUpdateItemGPU));

  const uint64_t tlasCopyBytes = static_cast<uint64_t>(tlasCwbvhNodeCount) * sizeof(Scene::PackedCWBVHNodeGPU);
  auto *copyTlasPass =
      renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests.world.copyTlas", 0u, tlasManager.getCwbvhNodesBuffer(), 0u, tlasCopyBytes, worldBlasBuffers.sceneCWBVHNodes, 0u, tlasCopyBytes);
  auto *transformPass = renderGraph.registerPass<WorldBLASPass>("SoftwareBVHTests.world.transformTriangles", 1u, WorldBLASPass::Stage::TransformTriangles, worldBlasBuffers, worldBlasSettings);
  auto *refitPass = renderGraph.registerPass<WorldBLASPass>("SoftwareBVHTests.world.refitBvh8", 1u, WorldBLASPass::Stage::RefitWorldBVH8, worldBlasBuffers, worldBlasSettings);
  auto *leafPass = renderGraph.registerPass<WorldBLASPass>("SoftwareBVHTests.world.copyLeafRemap", 1u, WorldBLASPass::Stage::CopyLeafRemap, worldBlasBuffers, worldBlasSettings);
  auto *compressPass = renderGraph.registerPass<WorldBLASPass>("SoftwareBVHTests.world.compressCwbvh", 2u, WorldBLASPass::Stage::CompressBVH8ToCWBVH, worldBlasBuffers, worldBlasSettings);

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = 1u;
  settings.height = 1u;
  settings.cameraPosition = cameraPosition;
  settings.cameraForward = cameraForward;
  makeCameraBasis(cameraForward, &settings.cameraRight, &settings.cameraUp);
  settings.verticalFovRadians = math::radians(60.0f);
  settings.tMin = 0.001f;
  settings.tMax = 100.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.primitiveMode = SWRTPass::PrimitiveMode::Triangle;
  settings.traversalMode = SWRTPass::TraversalMode::WorldCWBVH;
  settings.tlasMode = SWRTPass::TLASMode::CWBVH;
  settings.shaderVariant = SWRTPass::ShaderVariant::Debug;

  auto *tracePass = renderGraph.registerPass<SWRTPass>(
      "SoftwareBVHTests.world.raytrace",
      3u,
      tlasManager,
      blasManager,
      settings,
      SWRTPass::WorldCWBVHBufferSet{
        .sceneCWBVHNodes = worldBlasBuffers.sceneCWBVHNodes,
        .sceneCWBVHNodesSize = static_cast<uint64_t>(worldBlasSettings.maxSceneCWBVHNodeCount) * sizeof(Scene::PackedCWBVHNodeGPU),
        .worldTriangleTransforms = worldBlasBuffers.worldTriangles,
        .worldTriangleTransformsSize = static_cast<uint64_t>(worldBlasSettings.maxWorldTriangleCount) * sizeof(Scene::TriBaryTransformGPU),
        .worldLeafRemap = worldBlasBuffers.worldLeafRemap,
        .worldLeafRemapSize = static_cast<uint64_t>(worldBlasSettings.maxWorldLeafRemapCount) * sizeof(Scene::WorldLeafEntryGPU),
        .worldInstanceRanges = worldBlasBuffers.instanceRanges,
        .worldInstanceRangesSize = static_cast<uint64_t>(worldBlasSettings.maxInstanceRangeCount) * sizeof(Scene::WorldInstanceRangeGPU),
      });

  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.world.hitOutputReadback",
        .size = sizeof(SWRTPass::HitOutputGPU),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  const rendering::Buffer instanceRangeReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.world.instanceRangeReadback",
        .size = sizeof(Scene::WorldInstanceRangeGPU),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(
      "SoftwareBVHTests.world.hitOutputCopy", 4u, tracePass->getHitOutputBuffer(), 0u, sizeof(SWRTPass::HitOutputGPU), hitOutputReadback, 0u, sizeof(SWRTPass::HitOutputGPU));
  auto *instanceRangeReadbackPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(
      "SoftwareBVHTests.world.instanceRangeReadbackCopy", 4u, worldBlasBuffers.instanceRanges, 0u, sizeof(Scene::WorldInstanceRangeGPU), instanceRangeReadback, 0u, sizeof(Scene::WorldInstanceRangeGPU));

  runGraph(renderGraph);
  const Scene::WorldInstanceRangeGPU gpuRange = readBufferValue<Scene::WorldInstanceRangeGPU>(renderGraph, instanceRangeReadback);
  expectEqual(gpuRange.sceneInstanceId, 100u, "World CWBVH GPU range upload should preserve scene instance id");
  expectEqual(gpuRange.instanceIndex, 0u, "World CWBVH GPU range upload should preserve TLAS instance index");
  SWRTRayTraceResult result{};
  result.stats = readBufferValue<SWRTPass::StatsGPU>(renderGraph, tracePass->getStatsBuffer());
  result.hit = readBufferValue<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback);

  delete instanceRangeReadbackPass;
  delete hitOutputCopyPass;
  delete tracePass;
  delete compressPass;
  delete leafPass;
  delete refitPass;
  delete transformPass;
  delete copyTlasPass;
  delete uploadLeafItemsPass;
  delete uploadBvh8ItemsPass;
  delete uploadTriangleItemsPass;
  delete uploadTransformsPass;
  delete uploadRangesPass;
  return result;
}

void testSoftwareRayTraceSingleRayHitAndMiss(rendering::RHI &rhi)
{
  const SWRTRayTraceResult hitResult = runSingleTriangleRayTrace(rhi, math::Vec3f(0.0f, 0.0f, -1.0f), math::Vec3f(0.0f, 0.0f, -5.0f));
  expectEqual(hitResult.hit.instanceId != UINT32_MAX, true, "SWRT hit output should be valid");
  expectEqual(hitResult.hit.triangleId, 0u, "SWRT hit output should record triangle 0");
  expectEqual(hitResult.hit.instanceId, 42u, "SWRT hit output should record scene instance id");

  const SWRTRayTraceResult missResult = runSingleTriangleRayTrace(rhi, math::Vec3f(1.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -5.0f));
  expectEqual(missResult.hit.instanceId == UINT32_MAX, true, "SWRT miss output should be invalid");
  expectEqual(missResult.hit.triangleId, UINT32_MAX, "SWRT miss output should clear triangle id");
  expectEqual(missResult.hit.instanceId, UINT32_MAX, "SWRT miss output should clear scene instance id");

  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/raytrace_triangle_direct_fetch.mesh";
  blasInput.fallbackPath = "tests/raytrace_triangle_direct_fetch.fallback.mesh";
  blasInput.mesh = makeFrontFacingTriangleMesh(0.0f);
  const SWRTRayTraceResult directFetchResult = runSingleRayTraceForMesh(rhi, blasInput, math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f), math::Vec3f(0.0f, 0.0f, -5.0f));
  expectEqual(directFetchResult.hit.instanceId != UINT32_MAX, true, "SWRT direct ray id hit output should be valid");
  expectEqual(directFetchResult.hit.triangleId, 0u, "SWRT direct ray id path should keep closest-hit result");
  expectEqual(directFetchResult.stats.dynamicRayFetchCursor, 0u, "SWRT direct ray id path should not consume the dynamic fetch cursor");
}

void testSoftwareRayTraceExternalRaySorting(rendering::RHI &rhi)
{
  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/external_raytrace_triangle.mesh";
  blasInput.fallbackPath = "tests/external_raytrace_triangle.fallback.mesh";
  blasInput.mesh = makeFrontFacingTriangleMesh(0.0f);

  const std::vector<SWRTPass::TraceRayGPU> rays = {
    makeTraceRay(math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
    makeTraceRay(math::Vec3f(8.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
    makeTraceRay(math::Vec3f(0.1f, 0.1f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
    makeTraceRay(math::Vec3f(-8.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
  };

  const SWRTExternalRayTraceResult result = runExternalRayTraceForMesh(rhi, blasInput, rays, math::Vec3f(0.0f, 0.0f, -5.0f), true);
  expectEqual(static_cast<uint32_t>(result.hits.size()), static_cast<uint32_t>(rays.size()), "Sorted external SWRT should emit one hit record per source ray");
  expectEqual(result.hits[0u].instanceId, 42u, "Sorted external SWRT ray 0 should hit");
  expectEqual(result.hits[0u].triangleId, 0u, "Sorted external SWRT ray 0 should preserve triangle id");
  expectEqual(result.hits[1u].instanceId, UINT32_MAX, "Sorted external SWRT ray 1 should miss in original ray slot");
  expectEqual(result.hits[2u].instanceId, 42u, "Sorted external SWRT ray 2 should hit in original ray slot");
  expectEqual(result.hits[3u].instanceId, UINT32_MAX, "Sorted external SWRT ray 3 should miss in original ray slot");
}

void testSoftwareRayTraceStreamExternalRaysMatchLocalTrace(rendering::RHI &rhi)
{
  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/stream_external_raytrace_triangle.mesh";
  blasInput.fallbackPath = "tests/stream_external_raytrace_triangle.fallback.mesh";
  blasInput.mesh = makeSeparatedTrianglesMesh();

  const std::vector<SWRTPass::TraceRayGPU> rays = {
    makeTraceRay(math::Vec3f(-2.5f, 0.5f, 2.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
    makeTraceRay(math::Vec3f(2.5f, 0.5f, 2.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
    makeTraceRay(math::Vec3f(0.0f, 0.5f, 2.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
    makeTraceRay(math::Vec3f(-2.5f, 0.5f, 2.0f), math::Vec3f(0.2f, 0.0f, -1.0f).normalize()),
    makeTraceRay(math::Vec3f(2.5f, 0.5f, 2.0f), math::Vec3f(-0.2f, 0.0f, -1.0f).normalize()),
    makeTraceRay(math::Vec3f(8.0f, 0.0f, 2.0f), math::Vec3f(0.0f, 0.0f, -1.0f)),
  };

  const SWRTExternalRayTraceResult baseline = runExternalRayTraceForMesh(rhi, blasInput, rays, math::Vec3f(0.0f, 0.0f, -5.0f), false);
  const SWRTExternalRayTraceResult streamed = runExternalRayTraceForMesh(rhi, blasInput, rays, math::Vec3f(0.0f, 0.0f, -5.0f), false, true, 16u, 32u, 1u);
  std::cout << "  Stream external stats: total=" << streamed.stats.totalRays << " hit=" << streamed.stats.hitRays << " miss=" << streamed.stats.missRays
            << " tlasVisits=" << streamed.stats.tlasNodeVisits << " blasVisits=" << streamed.stats.blasNodeVisits << " triangleTests=" << streamed.stats.triangleTests
            << " candidates=" << streamed.stats.twoStageBLASCandidatesEmitted << " candidateTests=" << streamed.stats.twoStageBLASCandidatesTested
            << " pushes=" << streamed.stats.workgroupQueuePushes << " pops=" << streamed.stats.workgroupQueuePops << " spills=" << streamed.stats.localStackSpills
            << " hashSlots=" << streamed.stats.streamHashSlotsFlattened << " hashBlocks=" << streamed.stats.streamHashBlocksFlattened << " hashPackets=" << streamed.stats.streamHashPacketsEmitted
            << " streamOverflow=" << streamed.stats.streamQueueOverflows << " finalA=" << streamed.stats.streamFinalTaskCountA << " finalB=" << streamed.stats.streamFinalTaskCountB << "\n";
  expectTraceResultsMatch(streamed, baseline, "Stream external rays");
  expectEqual(streamed.stats.streamHashSlotsFlattened, 0u, "Stream external rays should bypass global hash slots");
  expectEqual(streamed.stats.streamHashBlocksFlattened, 0u, "Stream external rays should bypass hash packet blocks");
  expectEqual(streamed.stats.streamHashPacketsEmitted, 0u, "Stream external rays should bypass flattened stream packets");
  const uint32_t expectedHits = countTraceHits(baseline.hits);
  expectEqual(streamed.stats.totalRays, static_cast<uint32_t>(rays.size()), "Stream external rays stats total rays");
  expectEqual(streamed.stats.hitRays, expectedHits, "Stream external rays stats hit rays");
  expectEqual(streamed.stats.missRays, static_cast<uint32_t>(rays.size()) - expectedHits, "Stream external rays stats miss rays");
  expectEqual(streamed.stats.streamFinalTaskCountA + streamed.stats.streamFinalTaskCountB, 0u, "Stream external rays should finish all queued tasks");
  expectTrue(streamed.stats.twoStageBLASCandidatesEmitted > 0u, "Stream external rays should emit coarse BLAS candidates");
  expectTrue(streamed.stats.twoStageBLASCandidatesTested > 0u, "Stream external rays should test coarse BLAS candidates");
  expectTrue(streamed.stats.triangleTests > 0u, "Stream external rays should test triangles");
}

void testSoftwareRayTraceStreamDirectQueueProcessesAllMultiInstanceRays(rendering::RHI &rhi)
{
  const SWRTExternalRayTraceResult baseline = runMultiInstanceStreamHashTrace(rhi, false);
  const SWRTExternalRayTraceResult streamed = runMultiInstanceStreamHashTrace(rhi, true);
  std::cout << "  Stream multi-instance hash stats: total=" << streamed.stats.totalRays << " hit=" << streamed.stats.hitRays << " miss=" << streamed.stats.missRays
            << " tlasVisits=" << streamed.stats.tlasNodeVisits << " blasVisits=" << streamed.stats.blasNodeVisits << " triangleTests=" << streamed.stats.triangleTests
            << " candidates=" << streamed.stats.twoStageBLASCandidatesEmitted << " candidateTests=" << streamed.stats.twoStageBLASCandidatesTested
            << " pushes=" << streamed.stats.workgroupQueuePushes << " pops=" << streamed.stats.workgroupQueuePops << " spills=" << streamed.stats.localStackSpills
            << " hashSlots=" << streamed.stats.streamHashSlotsFlattened << " hashBlocks=" << streamed.stats.streamHashBlocksFlattened << " hashPackets=" << streamed.stats.streamHashPacketsEmitted
            << " streamOverflow=" << streamed.stats.streamQueueOverflows << " finalA=" << streamed.stats.streamFinalTaskCountA << " finalB=" << streamed.stats.streamFinalTaskCountB << "\n";

  expectTraceResultsMatch(streamed, baseline, "Stream multi-instance hash rays");
  expectEqual(countTraceHits(baseline.hits), 96u, "Multi-instance local trace should hit every ray");
  expectEqual(streamed.stats.totalRays, 96u, "Stream multi-instance hash stats total rays");
  expectEqual(streamed.stats.hitRays, 96u, "Stream multi-instance hash stats hit rays");
  expectEqual(streamed.stats.missRays, 0u, "Stream multi-instance hash stats miss rays");
  expectEqual(streamed.stats.streamHashSlotsFlattened, 0u, "Stream multi-instance direct queue should bypass hash slots");
  expectEqual(streamed.stats.streamHashBlocksFlattened, 0u, "Stream multi-instance direct queue should bypass hash blocks");
  expectEqual(streamed.stats.streamHashPacketsEmitted, 0u, "Stream multi-instance direct queue should bypass flattened packets");
  expectEqual(streamed.stats.streamFinalTaskCountA + streamed.stats.streamFinalTaskCountB, 0u, "Stream multi-instance direct queue should finish all queued tasks");
  expectTrue(streamed.stats.twoStageBLASCandidatesEmitted >= 96u, "Stream multi-instance should emit coarse BLAS candidates");
  expectTrue(streamed.stats.twoStageBLASCandidatesTested >= 96u, "Stream multi-instance should test coarse BLAS candidates");
}

SWRTExternalRayTraceResult runSponzaExternalRayTrace(
    rendering::RHI &rhi,
    const std::vector<virtualgeometry::FallbackMeshData> &fallbackMeshes,
    const std::vector<SWRTPass::TraceRayGPU> &rays,
    const bool enableRayStreamTraversal,
    const uint32_t streamMaxLocalNodeIterations = 8u,
    const uint32_t streamMaxDispatchPasses = 32u,
    const uint32_t streamTaskBudgetMultiplier = 128u)
{
  std::vector<BLASManager::BuildInput> blasInputs;
  std::vector<uint32_t> shapeToBlasInputIndex(fallbackMeshes.size(), UINT32_MAX);
  blasInputs.reserve(fallbackMeshes.size());
  uint32_t totalVertexCount = 0u;
  uint32_t totalIndexCount = 0u;
  uint32_t totalPrimitiveCount = 0u;
  uint32_t totalEstimatedNodeCount = 0u;
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(fallbackMeshes.size()); ++shapeIndex)
  {
    const virtualgeometry::FallbackMeshData &mesh = fallbackMeshes[shapeIndex];
    if (mesh.vertices.empty() || mesh.indices.empty())
      continue;

    shapeToBlasInputIndex[shapeIndex] = static_cast<uint32_t>(blasInputs.size());
    BLASManager::BuildInput input{};
    input.sourcePath = "tests/sponza_stream_trace_shape_" + std::to_string(shapeIndex) + ".mesh";
    input.fallbackPath = input.sourcePath + ".fallback";
    input.mesh = mesh;
    blasInputs.push_back(std::move(input));
    totalVertexCount += static_cast<uint32_t>(mesh.vertices.size());
    totalIndexCount += static_cast<uint32_t>(mesh.indices.size());
    const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
    totalPrimitiveCount += primitiveCount;
    totalEstimatedNodeCount += primitiveCount * 2u + 1u;
  }
  expectTrue(!blasInputs.empty(), "Sponza stream trace should have BLAS inputs");

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager::Settings blasSettings{};
  blasSettings.maxBlases = std::max<uint32_t>(static_cast<uint32_t>(blasInputs.size()), 1u);
  blasSettings.maxVertices = std::max<uint32_t>(totalVertexCount + 16u, 128u);
  blasSettings.maxIndices = std::max<uint32_t>(totalIndexCount + 32u, 256u);
  blasSettings.maxNodes = std::max<uint32_t>(totalEstimatedNodeCount + static_cast<uint32_t>(blasInputs.size()), 128u);
  blasSettings.maxPrimitives = std::max<uint32_t>(totalPrimitiveCount + static_cast<uint32_t>(blasInputs.size()), 64u);
  blasSettings.maxPackedCWBVHNodes = std::max<uint32_t>(totalEstimatedNodeCount + static_cast<uint32_t>(blasInputs.size()), 128u);
  blasSettings.maxRbvhDepthMipTexels = 64u;
  blasSettings.frameLocalBuffers = false;
  blasSettings.workgroupSize = 64u;
  BLASManager blasManager(&renderGraph, blasSettings);
  blasManager.syncFromCPU(blasInputs);

  std::vector<TLASManager::InstanceBuildInput> instances;
  instances.reserve(blasInputs.size());
  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(fallbackMeshes.size()); ++shapeIndex)
  {
    const uint32_t blasInputIndex = shapeToBlasInputIndex[shapeIndex];
    if (blasInputIndex == UINT32_MAX)
      continue;

    const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInputs[blasInputIndex].sourcePath);
    expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, "Sponza stream trace BLAS handle should be valid[" + std::to_string(shapeIndex) + "]");
    virtualgeometry::AABB localBounds{};
    expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), "Sponza stream trace local bounds should be available[" + std::to_string(shapeIndex) + "]");

    TLASManager::InstanceBuildInput instance{};
    instance.instanceId = 200000u + shapeIndex;
    instance.blasHandle = blasHandle;
    instance.transform = math::Mat4f::identity();
    instance.previousTransform = instance.transform;
    instance.worldBounds = localBounds;
    instance.sourceName = "sponza_stream_shape_" + std::to_string(shapeIndex);
    instance.sourcePath = blasInputs[blasInputIndex].sourcePath;
    instances.push_back(instance);
  }
  expectTrue(!instances.empty(), "Sponza stream trace should have TLAS instances");

  TLASManager::Settings tlasSettings = tlasTestSettings();
  tlasSettings.maxInstances = std::max<uint32_t>(static_cast<uint32_t>(instances.size()), tlasSettings.maxInstances);
  TLASManager tlasManager(&renderGraph, tlasSettings);
  tlasManager.rebuild(instances, static_cast<uint32_t>(instances.size()));

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = std::max<uint32_t>(static_cast<uint32_t>(rays.size()), 1u);
  settings.height = 1u;
  settings.generateCameraRays = false;
  settings.rayCount = static_cast<uint32_t>(rays.size());
  settings.tMin = 0.001f;
  settings.tMax = 10000.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.primitiveMode = SWRTPass::PrimitiveMode::Triangle;
  settings.tlasMode = SWRTPass::TLASMode::CWBVH;
  settings.enableRaySorting = false;
  settings.enableRayStreamTraversal = enableRayStreamTraversal;
  settings.streamMaxDispatchPasses = streamMaxDispatchPasses;
  settings.streamTaskBudgetMultiplier = streamTaskBudgetMultiplier;
  settings.streamMaxLocalNodeIterations = streamMaxLocalNodeIterations;
  settings.shaderVariant = enableRayStreamTraversal ? SWRTPass::ShaderVariant::Debug : SWRTPass::ShaderVariant::Release;

  auto *tracePass = renderGraph.registerPass<SWRTPass>(
      enableRayStreamTraversal ? "SoftwareBVHTests.sponzaStreamRaytrace" : "SoftwareBVHTests.sponzaLocalRaytrace", 1u, tlasManager, blasManager, settings);

  const uint64_t rayBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::TraceRayGPU);
  const rendering::Buffer rayUpload = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.sponzaRaytrace.rayUpload",
        .size = std::max<uint64_t>(rayBytes, 4u),
        .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  renderGraph.bufferWrite(rayUpload, 0u, rayBytes, const_cast<SWRTPass::TraceRayGPU *>(rays.data()));
  auto *rayUploadPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests.sponzaRaytrace.rayUploadCopy", 0u, rayUpload, 0u, rayBytes, tracePass->getRayInputBuffer(), 0u, rayBytes);

  const uint64_t hitBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::HitOutputGPU);
  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests.sponzaRaytrace.hitOutputReadback",
        .size = std::max<uint64_t>(hitBytes, 4u),
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass =
      renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests.sponzaRaytrace.hitOutputCopy", 2u, tracePass->getHitOutputBuffer(), 0u, hitBytes, hitOutputReadback, 0u, hitBytes);

  runGraph(renderGraph);
  SWRTExternalRayTraceResult result{};
  result.stats = readBufferValue<SWRTPass::StatsGPU>(renderGraph, tracePass->getStatsBuffer());
  result.hits = readBufferVector<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback, static_cast<uint32_t>(rays.size()));

  delete hitOutputCopyPass;
  delete rayUploadPass;
  delete tracePass;
  return result;
}

struct SponzaStreamRaySet
{
  std::vector<virtualgeometry::FallbackMeshData> meshes;
  std::vector<SWRTPass::TraceRayGPU> rays;
};

struct SponzaQuantizedTriangleTarget
{
  uint32_t sourceShapeIndex = 0u;
  TriangleRayTarget target{};
};

float meshDiagonal(const virtualgeometry::FallbackMeshData &mesh)
{
  return (math::Vec3f(mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]) - math::Vec3f(mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2])).length();
}

float quantizedTrianglePositionTolerance(const virtualgeometry::FallbackMeshData &mesh)
{
  return std::max(meshDiagonal(mesh) / 255.0f * 4.0f, 1.0e-3f);
}

void expectQuantizedTriangleTargetHit(
    const SWRTPass::HitOutputGPU &hit,
    const SponzaQuantizedTriangleTarget &expected,
    const virtualgeometry::FallbackMeshData &mesh,
    const uint32_t expectedInstanceId,
    const std::string &label)
{
  expectEqual(hit.instanceId, expectedInstanceId, label + " instance id");
  expectEqual(hit.tlasInstanceIndex, 0u, label + " TLAS instance index");
  expectEqual(hit.triangleId, expected.target.primitiveIndex, label + " primitive id");

  const math::Vec3f rayDirection = (expected.target.centroid - expected.target.cameraPosition).normalize();
  const math::Vec3f hitPoint = expected.target.cameraPosition + rayDirection * hit.hitT;
  const float expectedT = (expected.target.centroid - expected.target.cameraPosition).length();
  const float positionTolerance = quantizedTrianglePositionTolerance(mesh);
  expectNear(hit.hitT, expectedT, label + " hit distance", positionTolerance);
  expectTrue((hitPoint - expected.target.centroid).length() <= positionTolerance, label + " hit point should reconstruct near the targeted quantized triangle centroid");
  expectNear(hit.baryU, 1.0f / 3.0f, label + " bary u", 0.08f);
  expectNear(hit.baryV, 1.0f / 3.0f, label + " bary v", 0.08f);
  expectTrue(hit.baryU >= -0.02f && hit.baryV >= -0.02f && hit.baryU + hit.baryV <= 1.02f, label + " barycentrics should stay inside the triangle");
}

SponzaStreamRaySet buildSponzaStreamRaySet(const std::string &testName, const uint32_t maxMeshes, const uint32_t repeatsPerHitRay, const uint32_t missRayCount, const uint32_t minMeshes = 1u)
{
  const virtualgeometry::VirtualGeometrySourceAsset sourceAsset = loadObjSourceAssetForTest(sponzaAssetPath(), testName);
  const std::vector<virtualgeometry::FallbackMeshData> fallbackMeshes = virtualgeometry::FallbackMeshBuilder::buildOriginalPerShape(sourceAsset);
  expectTrue(!fallbackMeshes.empty(), "Sponza stream trace should build fallback meshes");

  SponzaStreamRaySet raySet{};
  virtualgeometry::AABB sceneBounds{};
  bool haveBounds = false;
  for (const virtualgeometry::FallbackMeshData &mesh : fallbackMeshes)
  {
    if (mesh.vertices.empty() || mesh.indices.empty())
      continue;

    const std::vector<TriangleRayTarget> targets = selectExteriorTraceTargetTriangles(mesh, 1u);
    if (targets.empty())
      continue;

    raySet.meshes.push_back(mesh);
    const virtualgeometry::AABB meshBox = meshBounds(mesh);
    if (!haveBounds)
    {
      sceneBounds = meshBox;
      haveBounds = true;
    }
    else
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        sceneBounds.minPoint[axis] = std::min(sceneBounds.minPoint[axis], meshBox.minPoint[axis]);
        sceneBounds.maxPoint[axis] = std::max(sceneBounds.maxPoint[axis], meshBox.maxPoint[axis]);
      }
    }
    const TriangleRayTarget &target = targets.front();
    const SWRTPass::TraceRayGPU hitRay = makeTraceRay(target.cameraPosition, (target.centroid - target.cameraPosition).normalize(), 10000.0f);
    for (uint32_t repeat = 0u; repeat < repeatsPerHitRay; ++repeat)
      raySet.rays.push_back(hitRay);
    if (raySet.meshes.size() >= maxMeshes)
      break;
  }
  expectTrue(static_cast<uint32_t>(raySet.meshes.size()) >= minMeshes, "Sponza stream trace should select enough representative meshes");
  expectTrue(!raySet.rays.empty(), "Sponza stream trace should find representative exterior hit rays");
  expectTrue(haveBounds, "Sponza stream trace should compute representative scene bounds");

  const math::Vec3f sceneCenter(
      (sceneBounds.minPoint[0] + sceneBounds.maxPoint[0]) * 0.5f, (sceneBounds.minPoint[1] + sceneBounds.maxPoint[1]) * 0.5f, (sceneBounds.minPoint[2] + sceneBounds.maxPoint[2]) * 0.5f);
  const float sceneSpan = std::max(
      {sceneBounds.maxPoint[0] - sceneBounds.minPoint[0], sceneBounds.maxPoint[1] - sceneBounds.minPoint[1], sceneBounds.maxPoint[2] - sceneBounds.minPoint[2], 1.0f});
  for (uint32_t missIndex = 0u; missIndex < missRayCount; ++missIndex)
  {
    const float offset = static_cast<float>(missIndex) * 0.25f;
    raySet.rays.push_back(makeTraceRay(
        sceneCenter + math::Vec3f(sceneSpan * 2.0f + offset, sceneSpan * 1.5f, sceneSpan * 2.0f),
        math::Vec3f(1.0f, 0.0f, 0.0f),
        sceneSpan));
  }

  return raySet;
}

void expectSponzaStreamMatchesLocal(
    rendering::RHI &rhi,
    const SponzaStreamRaySet &raySet,
    const std::string &label,
    const uint32_t streamMaxLocalNodeIterations = 8u)
{
  const SWRTExternalRayTraceResult baseline = runSponzaExternalRayTrace(rhi, raySet.meshes, raySet.rays, false);
  const SWRTExternalRayTraceResult streamed = runSponzaExternalRayTrace(rhi, raySet.meshes, raySet.rays, true, streamMaxLocalNodeIterations);
  std::cout << "  Stream Sponza stats: total=" << streamed.stats.totalRays << " hit=" << streamed.stats.hitRays << " miss=" << streamed.stats.missRays
            << " tlasVisits=" << streamed.stats.tlasNodeVisits << " blasVisits=" << streamed.stats.blasNodeVisits << " triangleTests=" << streamed.stats.triangleTests
            << " candidates=" << streamed.stats.twoStageBLASCandidatesEmitted << " candidateTests=" << streamed.stats.twoStageBLASCandidatesTested
            << " pushes=" << streamed.stats.workgroupQueuePushes << " pops=" << streamed.stats.workgroupQueuePops << " spills=" << streamed.stats.localStackSpills
            << " hashSlots=" << streamed.stats.streamHashSlotsFlattened << " hashBlocks=" << streamed.stats.streamHashBlocksFlattened << " hashPackets=" << streamed.stats.streamHashPacketsEmitted
            << " streamOverflow=" << streamed.stats.streamQueueOverflows << " finalA=" << streamed.stats.streamFinalTaskCountA << " finalB=" << streamed.stats.streamFinalTaskCountB << "\n";
  expectTraceResultsMatch(streamed, baseline, label);
  const uint32_t expectedHits = countTraceHits(baseline.hits);
  expectTrue(expectedHits > 0u, "Sponza local trace should hit at least one ray");
  expectTrue(expectedHits < static_cast<uint32_t>(raySet.rays.size()), "Sponza local trace should miss at least one ray");
  expectEqual(streamed.stats.totalRays, static_cast<uint32_t>(raySet.rays.size()), label + " stats total rays");
  expectEqual(streamed.stats.hitRays, expectedHits, label + " stats hit rays");
  expectEqual(streamed.stats.missRays, static_cast<uint32_t>(raySet.rays.size()) - expectedHits, label + " stats miss rays");
  expectEqual(streamed.stats.streamFinalTaskCountA + streamed.stats.streamFinalTaskCountB, 0u, label + " should finish all queued stream tasks");
  expectTrue(streamed.stats.triangleTests > 0u, "Sponza stream trace should test triangles");
  expectTrue(streamed.stats.twoStageBLASCandidatesEmitted > 0u, "Sponza stream trace should emit coarse BLAS candidates");
  expectTrue(streamed.stats.twoStageBLASCandidatesTested > 0u, "Sponza stream trace should test coarse BLAS candidates");
  expectEqual(streamed.stats.streamHashSlotsFlattened, 0u, label + " should bypass global hash slots");
  expectEqual(streamed.stats.streamHashBlocksFlattened, 0u, label + " should bypass hash packet blocks");
  expectEqual(streamed.stats.streamHashPacketsEmitted, 0u, label + " should bypass flattened packets");
  expectTrue(streamed.stats.localStackSpills < static_cast<uint32_t>(raySet.rays.size()), "Sponza stream trace should keep queue spills bounded");
}

void testSoftwareRayTraceStreamSponzaMatchesLocalTrace(rendering::RHI &rhi)
{
  const SponzaStreamRaySet raySet = buildSponzaStreamRaySet("sponza_stream_trace", 1u, 32u, 224u);
  expectSponzaStreamMatchesLocal(rhi, raySet, "Stream Sponza rays");
}

void testSoftwareRayTraceStreamSponzaDirectQueueMatchesLocalTrace(rendering::RHI &rhi)
{
  const SponzaStreamRaySet raySet = buildSponzaStreamRaySet("sponza_stream_hash_trace", 1u, 64u, 64u);
  expectSponzaStreamMatchesLocal(rhi, raySet, "Stream Sponza direct queue rays", 1u);
}

void testSoftwareRayTraceStreamSponzaMissOnlyMatchesLocalTrace(rendering::RHI &rhi)
{
  SponzaStreamRaySet raySet = buildSponzaStreamRaySet("sponza_stream_miss_only_trace", 1u, 1u, 64u);
  expectTrue(raySet.rays.size() > 1u, "Sponza miss-only stream trace should build at least one hit ray and miss rays");
  raySet.rays.erase(raySet.rays.begin());

  const SWRTExternalRayTraceResult baseline = runSponzaExternalRayTrace(rhi, raySet.meshes, raySet.rays, false);
  const SWRTExternalRayTraceResult streamed = runSponzaExternalRayTrace(rhi, raySet.meshes, raySet.rays, true);
  std::cout << "  Stream Sponza miss-only stats: total=" << streamed.stats.totalRays << " hit=" << streamed.stats.hitRays << " miss=" << streamed.stats.missRays
            << " tlasVisits=" << streamed.stats.tlasNodeVisits << " blasVisits=" << streamed.stats.blasNodeVisits << " triangleTests=" << streamed.stats.triangleTests
            << " candidates=" << streamed.stats.twoStageBLASCandidatesEmitted << " candidateTests=" << streamed.stats.twoStageBLASCandidatesTested
            << " pushes=" << streamed.stats.workgroupQueuePushes << " pops=" << streamed.stats.workgroupQueuePops << " spills=" << streamed.stats.localStackSpills
            << " hashSlots=" << streamed.stats.streamHashSlotsFlattened << " hashBlocks=" << streamed.stats.streamHashBlocksFlattened << " hashPackets=" << streamed.stats.streamHashPacketsEmitted
            << " streamOverflow=" << streamed.stats.streamQueueOverflows << " finalA=" << streamed.stats.streamFinalTaskCountA << " finalB=" << streamed.stats.streamFinalTaskCountB << "\n";
  expectTraceResultsMatch(streamed, baseline, "Stream Sponza miss-only rays", false);
  expectEqual(streamed.stats.streamFinalTaskCountA + streamed.stats.streamFinalTaskCountB, 0u, "Stream Sponza miss-only should finish all queued stream tasks");
  expectEqual(countTraceHits(baseline.hits), 0u, "Sponza miss-only local trace should miss every ray");
  expectEqual(streamed.stats.totalRays, static_cast<uint32_t>(raySet.rays.size()), "Stream Sponza miss-only stats total rays");
  expectEqual(streamed.stats.hitRays, 0u, "Stream Sponza miss-only stats hit rays");
  expectEqual(streamed.stats.missRays, static_cast<uint32_t>(raySet.rays.size()), "Stream Sponza miss-only stats miss rays");
  expectEqual(streamed.stats.twoStageBLASCandidatesTested, 0u, "Sponza miss-only stream trace should not test BLAS candidates");
}

void testSoftwareRayTraceSponzaQuantizedTriangleTargets(rendering::RHI &rhi)
{
  const virtualgeometry::VirtualGeometrySourceAsset sourceAsset = loadObjSourceAssetForTest(sponzaAssetPath(), "sponza_quantized_triangle_targets");
  const std::vector<virtualgeometry::FallbackMeshData> fallbackMeshes = virtualgeometry::FallbackMeshBuilder::buildOriginalPerShape(sourceAsset);
  expectTrue(!fallbackMeshes.empty(), "Sponza quantized triangle target trace should build fallback meshes");

  constexpr uint32_t kMaxInstances = 3u;
  constexpr uint32_t kTargetsPerInstance = 2u;
  uint32_t testedInstances = 0u;
  uint32_t testedTriangles = 0u;

  for (uint32_t sourceShapeIndex = 0u; sourceShapeIndex < static_cast<uint32_t>(fallbackMeshes.size()) && testedInstances < kMaxInstances; ++sourceShapeIndex)
  {
    const virtualgeometry::FallbackMeshData &mesh = fallbackMeshes[sourceShapeIndex];
    if (mesh.vertices.empty() || mesh.indices.empty())
      continue;

    const std::vector<TriangleRayTarget> traceTargets = selectExteriorTraceTargetTriangles(mesh, kTargetsPerInstance);
    if (traceTargets.empty())
      continue;

    std::vector<SWRTPass::TraceRayGPU> rays;
    std::vector<SponzaQuantizedTriangleTarget> expectedTargets;
    rays.reserve(traceTargets.size());
    expectedTargets.reserve(traceTargets.size());
    for (const TriangleRayTarget &target : traceTargets)
    {
      const math::Vec3f rayDirection = (target.centroid - target.cameraPosition).normalize();
      rays.push_back(makeTraceRay(target.cameraPosition, rayDirection, 10000.0f));
      expectedTargets.push_back(SponzaQuantizedTriangleTarget{.sourceShapeIndex = sourceShapeIndex, .target = target});
    }

    const std::vector<virtualgeometry::FallbackMeshData> singleInstanceMesh = {mesh};
    const SWRTExternalRayTraceResult localTrace = runSponzaExternalRayTrace(rhi, singleInstanceMesh, rays, false);
    const SWRTExternalRayTraceResult streamTrace = runSponzaExternalRayTrace(rhi, singleInstanceMesh, rays, true);
    expectEqual(static_cast<uint32_t>(localTrace.hits.size()), static_cast<uint32_t>(expectedTargets.size()), "Sponza quantized local trace hit count");
    expectEqual(static_cast<uint32_t>(streamTrace.hits.size()), static_cast<uint32_t>(expectedTargets.size()), "Sponza quantized stream trace hit count");
    expectEqual(countTraceHits(localTrace.hits), static_cast<uint32_t>(expectedTargets.size()), "Sponza quantized local trace should hit every targeted triangle");
    expectEqual(countTraceHits(streamTrace.hits), static_cast<uint32_t>(expectedTargets.size()), "Sponza quantized stream trace should hit every targeted triangle");

    for (uint32_t targetIndex = 0u; targetIndex < static_cast<uint32_t>(expectedTargets.size()); ++targetIndex)
    {
      const std::string label =
          "Sponza quantized target shape[" + std::to_string(sourceShapeIndex) + "] triangle[" + std::to_string(expectedTargets[targetIndex].target.primitiveIndex) + "]";
      if (localTrace.hits[targetIndex].triangleId != expectedTargets[targetIndex].target.primitiveIndex ||
          streamTrace.hits[targetIndex].triangleId != expectedTargets[targetIndex].target.primitiveIndex)
      {
        std::cout << "  Quantized target mismatch " << label
                  << " localTri=" << localTrace.hits[targetIndex].triangleId
                  << " localT=" << localTrace.hits[targetIndex].hitT
                  << " localBary=(" << localTrace.hits[targetIndex].baryU << "," << localTrace.hits[targetIndex].baryV << ")"
                  << " streamTri=" << streamTrace.hits[targetIndex].triangleId
                  << " streamT=" << streamTrace.hits[targetIndex].hitT
                  << " streamBary=(" << streamTrace.hits[targetIndex].baryU << "," << streamTrace.hits[targetIndex].baryV << ")"
                  << " expectedT=" << (expectedTargets[targetIndex].target.centroid - expectedTargets[targetIndex].target.cameraPosition).length()
                  << " streamOverflow=" << streamTrace.stats.streamQueueOverflows
                  << " finalA=" << streamTrace.stats.streamFinalTaskCountA
                  << " finalB=" << streamTrace.stats.streamFinalTaskCountB
                  << "\n";
      }
      expectQuantizedTriangleTargetHit(localTrace.hits[targetIndex], expectedTargets[targetIndex], mesh, 200000u, label + " local");
      expectQuantizedTriangleTargetHit(streamTrace.hits[targetIndex], expectedTargets[targetIndex], mesh, 200000u, label + " stream");
      ++testedTriangles;
    }

    ++testedInstances;
  }

  expectTrue(testedInstances >= 2u, "Sponza quantized triangle target trace should cover multiple individual instances");
  expectTrue(testedTriangles >= 4u, "Sponza quantized triangle target trace should cover multiple individual triangles");
  std::cout << "  Sponza quantized triangle targets: instances=" << testedInstances << " triangles=" << testedTriangles << "\n";
}

void testSoftwareRayTraceMultiInstanceTlas(rendering::RHI &rhi)
{
  const std::vector<math::Vec3f> translations = {
    math::Vec3f(-2.0f, 0.0f, -5.0f),
    math::Vec3f(0.0f, 0.0f, -5.0f),
    math::Vec3f(2.0f, 0.0f, -5.0f),
  };

  const SWRTRayTraceResult centerResult = runMultiInstanceRayTrace(rhi, translations, math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f));
  expectEqual(centerResult.hit.instanceId != UINT32_MAX, true, "SWRT multi-instance TLAS should hit center instance");
  expectEqual(centerResult.hit.instanceId, 101u, "SWRT multi-instance TLAS should report center scene instance");

  const SWRTRayTraceResult leftResult = runMultiInstanceRayTrace(rhi, translations, math::Vec3f(-2.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f));
  expectEqual(leftResult.hit.instanceId != UINT32_MAX, true, "SWRT multi-instance TLAS should hit left instance");
  expectEqual(leftResult.hit.instanceId, 100u, "SWRT multi-instance TLAS should report left scene instance");

  const SWRTRayTraceResult rightResult = runMultiInstanceRayTrace(rhi, translations, math::Vec3f(2.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f));
  expectEqual(rightResult.hit.instanceId != UINT32_MAX, true, "SWRT multi-instance TLAS should hit right instance");
  expectEqual(rightResult.hit.instanceId, 102u, "SWRT multi-instance TLAS should report right scene instance");
}

void testSoftwareRayTraceWorldCwbvhSurvivesInstanceUpdateAndGraphRecompile(rendering::RHI &rhi)
{
  const SWRTRayTraceResult initialResult = runWorldCwbvhTriangleRayTrace(rhi, {math::Vec3f(0.0f, 0.0f, -5.0f)}, math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f));
  expectEqual(initialResult.hit.instanceId != UINT32_MAX, true, "World CWBVH trace should hit before instance update");
  expectEqual(initialResult.hit.instanceId, 100u, "World CWBVH trace should report the initial scene instance");
  expectEqual(initialResult.hit.triangleId, 0u, "World CWBVH trace should preserve triangle id before instance update");
  expectTrue(initialResult.stats.worldBlasNodeVisits > 0u, "World CWBVH trace should visit world BLAS nodes before instance update");

  const SWRTRayTraceResult movedResult = runWorldCwbvhTriangleRayTrace(rhi, {math::Vec3f(2.0f, 0.0f, -5.0f)}, math::Vec3f(2.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f));
  expectEqual(movedResult.hit.instanceId != UINT32_MAX, true, "World CWBVH trace should hit after instance update and graph recompile");
  expectEqual(movedResult.hit.instanceId, 100u, "World CWBVH trace should report the moved scene instance after graph recompile");
  expectEqual(movedResult.hit.triangleId, 0u, "World CWBVH trace should preserve triangle id after graph recompile");
  expectTrue(movedResult.stats.worldBlasNodeVisits > 0u, "World CWBVH trace should visit world BLAS nodes after graph recompile");

  const SWRTRayTraceResult movedMissFromOldCamera = runWorldCwbvhTriangleRayTrace(rhi, {math::Vec3f(2.0f, 0.0f, -5.0f)}, math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f));
  expectEqual(movedMissFromOldCamera.hit.instanceId, UINT32_MAX, "World CWBVH trace should use updated world-space instance placement");
}

void testSoftwareRayTraceWorldCwbvhReportsOriginalTriangleIds(rendering::RHI &rhi)
{
  const virtualgeometry::FallbackMeshData mesh = makeLeafBeforeInternalChildTrianglesMesh();
  const softwarebvh::CWBVHBuilder::BuildResult build = softwarebvh::CWBVHBuilder::build(mesh);
  verifyRtBuildCoversAllTriangles(build, mesh, "World CWBVH triangle id regression mesh");

  const math::Vec3f instanceTranslation(0.0f, 0.0f, -5.0f);
  const std::array<uint32_t, 3u> primitiveIndices = {0u, 4u, 8u};
  for (const uint32_t primitiveIndex : primitiveIndices)
  {
    const TriangleRayTarget target = makePrimitiveTraceTarget(mesh, primitiveIndex, 2.0f);
    const math::Vec3f cameraForward = (target.centroid - target.cameraPosition).normalize();
    const SWRTRayTraceResult result = runWorldCwbvhTriangleRayTrace(rhi, {instanceTranslation}, target.cameraPosition + instanceTranslation, cameraForward, &mesh);
    expectEqual(result.hit.instanceId != UINT32_MAX, true, "World CWBVH remapped primitive trace should hit");
    expectEqual(result.hit.instanceId, 100u, "World CWBVH remapped primitive trace should report scene instance");
    expectEqual(result.hit.triangleId, primitiveIndex, "World CWBVH should report original source triangle id[" + std::to_string(primitiveIndex) + "]");
    expectTrue(result.stats.worldBlasNodeVisits > 0u, "World CWBVH remapped primitive trace should visit world BLAS nodes");
  }
}

void testSoftwareRayTraceBvh4Tlas(rendering::RHI &rhi)
{
  const SWRTRayTraceResult hitResult = runSingleTriangleRayTrace(rhi, math::Vec3f(0.0f, 0.0f, -1.0f), math::Vec3f(0.0f, 0.0f, -5.0f), SWRTPass::TLASMode::BVH4);
  expectEqual(hitResult.hit.instanceId, 42u, "SWRT BVH4 TLAS should hit the single scene instance");
  expectEqual(hitResult.hit.triangleId, 0u, "SWRT BVH4 TLAS should preserve triangle id output");

  const SWRTRayTraceResult missResult = runSingleTriangleRayTrace(rhi, math::Vec3f(1.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -5.0f), SWRTPass::TLASMode::BVH4);
  expectEqual(missResult.hit.instanceId, UINT32_MAX, "SWRT BVH4 TLAS miss should clear scene instance id");
  expectEqual(missResult.hit.triangleId, UINT32_MAX, "SWRT BVH4 TLAS miss should clear triangle id");

  const std::vector<math::Vec3f> translations = {
    math::Vec3f(-2.0f, 0.0f, -5.0f),
    math::Vec3f(0.0f, 0.0f, -5.0f),
    math::Vec3f(2.0f, 0.0f, -5.0f),
  };
  const SWRTRayTraceResult centerResult = runMultiInstanceRayTrace(rhi, translations, math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f), SWRTPass::TLASMode::BVH4);
  expectEqual(centerResult.hit.instanceId, 101u, "SWRT BVH4 TLAS should report the center instance");
}

struct SWRTTraceBenchmarkResult
{
  double averageTraceMs = 0.0;
  double warmupTraceMs = 0.0;
  uint32_t measuredRuns = 0u;
  uint32_t rayCount = 0u;
  uint32_t instanceCount = 0u;
  uint32_t hitCount = 0u;
  uint32_t tlasCwbvhNodeCount = 0u;
  uint32_t tlasBvh4NodeCount = 0u;
  SWRTPass::StatsGPU stats{};
};

SWRTTraceBenchmarkResult runTlasTraceBenchmark(rendering::RHI &rhi, const SWRTPass::TLASMode tlasMode, const std::string &label)
{
  constexpr uint32_t kGridSize = 32u;
  constexpr uint32_t kInstanceCount = kGridSize * kGridSize;
  constexpr uint32_t kRayCount = 262144u;
  constexpr uint32_t kWarmupRuns = 2u;
  constexpr uint32_t kMeasuredRuns = 8u;
  constexpr float kInstanceSpacing = 2.0f;
  constexpr float kInstanceZ = -64.0f;

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/tlas_trace_benchmark_triangle.mesh";
  blasInput.fallbackPath = "tests/tlas_trace_benchmark_triangle.fallback.mesh";
  blasInput.mesh = makeFrontFacingTriangleMesh(0.0f);

  BLASManager blasManager(&renderGraph, blasTestSettingsForMesh(blasInput.mesh, nullptr));
  TLASManager::Settings tlasSettings = tlasTestSettings();
  tlasSettings.maxInstances = kInstanceCount;
  TLASManager tlasManager(&renderGraph, tlasSettings);
  blasManager.syncFromCPU({blasInput});

  const uint32_t blasHandle = blasManager.findHandleBySourcePath(blasInput.sourcePath);
  expectTrue(blasHandle != BLASManager::INVALID_BLAS_HANDLE, label + " BLAS handle should be valid");

  virtualgeometry::AABB localBounds{};
  expectTrue(blasManager.tryGetLocalBounds(blasHandle, &localBounds), label + " local bounds lookup should succeed");

  std::vector<TLASManager::InstanceBuildInput> instances;
  instances.reserve(kInstanceCount);
  for (uint32_t instanceIndex = 0u; instanceIndex < kInstanceCount; ++instanceIndex)
  {
    const uint32_t x = instanceIndex % kGridSize;
    const uint32_t y = instanceIndex / kGridSize;
    const math::Vec3f translation(
        (static_cast<float>(x) - static_cast<float>(kGridSize - 1u) * 0.5f) * kInstanceSpacing, (static_cast<float>(y) - static_cast<float>(kGridSize - 1u) * 0.5f) * kInstanceSpacing, kInstanceZ);

    TLASManager::InstanceBuildInput instance{};
    instance.instanceId = 10000u + instanceIndex;
    instance.blasHandle = blasHandle;
    instance.transform = makeTranslation(translation);
    instance.previousTransform = instance.transform;
    instance.worldBounds = translateBounds(localBounds, translation);
    instance.sourceName = "benchmarkTriangle" + std::to_string(instanceIndex);
    instance.sourcePath = "tests/tlas_trace_benchmark_triangle";
    instances.push_back(instance);
  }
  tlasManager.rebuild(instances, static_cast<uint32_t>(instances.size()));

  std::vector<SWRTPass::TraceRayGPU> rays(kRayCount);
  for (uint32_t rayIndex = 0u; rayIndex < kRayCount; ++rayIndex)
  {
    const uint32_t targetInstance = (rayIndex / 64u) % kInstanceCount;
    const virtualgeometry::AABB &bounds = instances[targetInstance].worldBounds;
    const float centerX = (bounds.minPoint[0] + bounds.maxPoint[0]) * 0.5f;
    const float centerY = (bounds.minPoint[1] + bounds.maxPoint[1]) * 0.5f;
    rays[rayIndex].originTMin[0] = centerX;
    rays[rayIndex].originTMin[1] = centerY;
    rays[rayIndex].originTMin[2] = 0.0f;
    rays[rayIndex].originTMin[3] = 0.001f;
    rays[rayIndex].directionTMax[0] = 0.0f;
    rays[rayIndex].directionTMax[1] = 0.0f;
    rays[rayIndex].directionTMax[2] = -1.0f;
    rays[rayIndex].directionTMax[3] = 256.0f;
  }

  SWRTPass::Settings settings{};
  settings.enabled = true;
  settings.width = 16384u;
  settings.height = kRayCount / settings.width;
  settings.generateCameraRays = false;
  settings.rayCount = kRayCount;
  settings.tMin = 0.001f;
  settings.tMax = 256.0f;
  settings.debugMode = SWRTPass::DebugMode::TriangleId;
  settings.primitiveMode = SWRTPass::PrimitiveMode::Triangle;
  settings.tlasMode = tlasMode;
  settings.enableRaySorting = false;
  settings.shaderVariant = SWRTPass::ShaderVariant::Release;

  auto *tracePass = renderGraph.registerPass<SWRTPass>("SoftwareBVHTests." + label + ".raytrace", 1u, tlasManager, blasManager, settings);

  const uint64_t rayBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::TraceRayGPU);
  const rendering::Buffer rayUpload = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests." + label + ".rayUpload",
        .size = rayBytes,
        .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  renderGraph.bufferWrite(rayUpload, 0u, rayBytes, rays.data());
  auto *rayUploadPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests." + label + ".rayUploadCopy", 0u, rayUpload, 0u, rayBytes, tracePass->getRayInputBuffer(), 0u, rayBytes);
  const uint64_t hitBytes = static_cast<uint64_t>(rays.size()) * sizeof(SWRTPass::HitOutputGPU);
  const rendering::Buffer hitOutputReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
        .name = "SoftwareBVHTests." + label + ".hitOutputReadback",
        .size = hitBytes,
        .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  auto *hitOutputCopyPass = renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>("SoftwareBVHTests." + label + ".hitOutputCopy", 2u, tracePass->getHitOutputBuffer(), 0u, hitBytes, hitOutputReadback, 0u, hitBytes);

  tracePass->requestTraceDispatchTimer();
  renderGraph.compile();
  tracePass->ensureTraceDispatchTimerAllocated();
  double warmupMs = 0.0;
  double measuredMs = 0.0;
  for (uint32_t runIndex = 0u; runIndex < kWarmupRuns + kMeasuredRuns; ++runIndex)
  {
    rendering::RenderGraph::Frame frame{};
    rendering::RenderGraph::Overrides overrides{};
    renderGraph.run(frame, overrides);
    renderGraph.waitFrame(frame);

    const double traceMs = renderGraph.readTimer(tracePass->getTraceDispatchTimer());
    if (runIndex < kWarmupRuns)
    {
      warmupMs += traceMs;
    }
    else
    {
      measuredMs += traceMs;
    }
  }

  SWRTTraceBenchmarkResult result{};
  result.averageTraceMs = measuredMs / static_cast<double>(kMeasuredRuns);
  result.warmupTraceMs = warmupMs / static_cast<double>(kWarmupRuns);
  result.measuredRuns = kMeasuredRuns;
  result.rayCount = kRayCount;
  result.instanceCount = kInstanceCount;
  result.tlasCwbvhNodeCount = tlasManager.getCwbvhNodeCount();
  result.tlasBvh4NodeCount = tlasManager.getBvh4NodeCount();
  const std::vector<SWRTPass::HitOutputGPU> hits = readBufferVector<SWRTPass::HitOutputGPU>(renderGraph, hitOutputReadback, static_cast<uint32_t>(rays.size()));
  for (const SWRTPass::HitOutputGPU &hit : hits)
  {
    if (hit.instanceId != UINT32_MAX)
    {
      ++result.hitCount;
    }
  }

  delete hitOutputCopyPass;
  delete rayUploadPass;
  delete tracePass;
  return result;
}

void testSoftwareRayTraceTlasTraceBenchmark(rendering::RHI &rhi)
{
  const SWRTTraceBenchmarkResult cwbvh = runTlasTraceBenchmark(rhi, SWRTPass::TLASMode::CWBVH, "tlasCwbvhBenchmark");
  const SWRTTraceBenchmarkResult bvh4 = runTlasTraceBenchmark(rhi, SWRTPass::TLASMode::BVH4, "tlasBvh4Benchmark");

  std::cout << "[SWRT TLAS trace benchmark] rays=" << cwbvh.rayCount << " instances=" << cwbvh.instanceCount << " measuredRuns=" << cwbvh.measuredRuns << "\n";
  std::cout << "  CWBVH-from-BVH4: avgTraceMs=" << cwbvh.averageTraceMs << " warmupAvgMs=" << cwbvh.warmupTraceMs << " tlasCwbvhNodes=" << cwbvh.tlasCwbvhNodeCount << " tlasBvh4Nodes=" << cwbvh.tlasBvh4NodeCount
            << " hitOutputs=" << cwbvh.hitCount << "\n";
  std::cout << "  BVH4:            avgTraceMs=" << bvh4.averageTraceMs << " warmupAvgMs=" << bvh4.warmupTraceMs << " tlasBvh4Nodes=" << bvh4.tlasBvh4NodeCount << " hitOutputs=" << bvh4.hitCount << "\n";

  expectTrue(cwbvh.hitCount > cwbvh.rayCount / 2u, "CWBVH TLAS benchmark should produce many hits");
  expectTrue(bvh4.hitCount > bvh4.rayCount / 2u, "BVH4 TLAS benchmark should produce many hits");
  expectTrue(cwbvh.tlasCwbvhNodeCount > 0u, "CWBVH TLAS benchmark should build packed nodes");
  expectTrue(cwbvh.tlasBvh4NodeCount > 0u, "CWBVH TLAS benchmark should build SAH BVH4 nodes");
}

void testGpuBuiltCwbvhTraceAndBounds(rendering::RHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager manager(&renderGraph, blasTestSettings());
  const virtualgeometry::FallbackMeshData mesh = makeSeparatedTrianglesMesh();

  BLASManager::BuildInput input{};
  input.sourcePath = "tests/separated_triangles_gpu_trace.mesh";
  input.fallbackPath = "tests/separated_triangles_gpu_trace.fallback.mesh";
  input.mesh = mesh;
  manager.syncFromCPU({input});

  const uint32_t handle = manager.findHandleBySourcePath(input.sourcePath);
  BLASManager::GPUBuildDescriptor descriptor{};
  expectTrue(manager.tryGetGPUBuildDescriptor(handle, &descriptor), "GPU trace BLAS descriptor lookup should succeed");

  auto *preparePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUPreparePass>("SoftwareBVHTests.prepareTraceBounds", 0u, manager, handle);
  auto *finalizePass = renderGraph.registerPass<softwarebvh::rendering::SoftwareBLASGPUFinalizePass>("SoftwareBVHTests.finalizeTraceBounds", 1u, manager, handle);
  runGraph(renderGraph);

  const auto gpuCwbvh =
      readBufferVector<BLASManager::CWBVHNodeGPU>(renderGraph, manager.getGPUCWBVHNodesBuffer(), descriptor.gpuCWBVHNodeCount, static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(BLASManager::CWBVHNodeGPU));
  const auto gpuLeafRemap = readBufferVector<uint32_t>(renderGraph, manager.getGPULeafRemapBuffer(), descriptor.primitiveCount, static_cast<uint64_t>(descriptor.gpuLeafRemapOffset) * sizeof(uint32_t));
  expectTrue(!gpuCwbvh.empty(), "GPU-built CWBVH should produce at least one packed node");

  const auto leafSlots = collectPackedLeafSlots(gpuCwbvh.front(), gpuLeafRemap);
  expectEqual(static_cast<uint32_t>(leafSlots.size()), descriptor.primitiveCount, "GPU-built CWBVH root should expose all primitive leaves");
  for (const auto &[childSlot, primitiveIndex] : leafSlots)
  {
    const virtualgeometry::AABB decodedLeaf = decodePackedChildBounds(gpuCwbvh.front(), childSlot);
    const virtualgeometry::AABB shaderDecodedLeaf = decodePackedChildBoundsAsShaderVec4s(gpuCwbvh.front(), childSlot);
    const virtualgeometry::AABB expectedLeaf = triangleBounds(mesh, primitiveIndex);
    compareBounds(shaderDecodedLeaf, decodedLeaf, "GPU-built BLAS shader-view packed child bounds");
    expectBoundsEncloses(shaderDecodedLeaf, expectedLeaf, "GPU-built packed child should enclose expected triangle bounds");
  }

  delete finalizePass;
  delete preparePass;

  const std::array<TriangleRayTarget, 2> rayTargets = {
    makePrimitiveTraceTarget(mesh, 0u, 2.0f),
    makePrimitiveTraceTarget(mesh, 1u, 2.0f),
  };
  for (const TriangleRayTarget &rayTarget : rayTargets)
  {
    const math::Vec3f cameraForward = (rayTarget.centroid - rayTarget.cameraPosition).normalize();
    const SWRTRayTraceResult result = runSingleRayTraceForMesh(rhi, input, rayTarget.cameraPosition, cameraForward, math::Vec3f(0.0f, 0.0f, -5.0f), true);
    expectEqual(result.hit.instanceId != UINT32_MAX, true, "GPU-built CWBVH trace output should be valid");
    expectEqual(result.hit.triangleId, rayTarget.primitiveIndex, "GPU-built CWBVH trace should hit the targeted primitive");
    expectEqual(result.hit.instanceId, 42u, "GPU-built CWBVH trace should record scene instance id");
  }

  const SWRTRayTraceResult missResult = runSingleRayTraceForMesh(rhi, input, math::Vec3f(0.0f, 0.0f, 0.0f), math::Vec3f(0.0f, 0.0f, -1.0f), math::Vec3f(0.0f, 0.0f, -5.0f), true);
  expectEqual(missResult.hit.instanceId == UINT32_MAX, true, "GPU-built CWBVH miss output should be invalid");
}

void testRbvhTraceKernelMatchesTriangleCwbvhBunnyRays(rendering::RHI &rhi)
{
  const virtualgeometry::FallbackMeshData bunny = loadObjFallbackMesh(stanfordBunnyAssetPath());
  const softwarebvh::CWBVHBuilder::BuildResult triangleBuild = softwarebvh::CWBVHBuilder::build(bunny);

  softwarebvh::RBVHBuildOptions rbvhOptions{};
  rbvhOptions.minCardResolution = 16u;
  rbvhOptions.maxCardResolution = 64u;
  rbvhOptions.maxAtlasSize = 4096u;
  rbvhOptions.projectedAreaThreshold = 0.75f;
  const softwarebvh::RBVHBuildResult rbvhBuild = softwarebvh::RBVHBuilder::build(bunny, triangleBuild, rbvhOptions);
  expectTrue(!rbvhBuild.cards.empty(), "RBVH bunny build should produce cards");

  BLASManager::BuildInput triangleInput{};
  triangleInput.sourcePath = "tests/stanford_bunny_triangle_trace.mesh";
  triangleInput.fallbackPath = "tests/stanford_bunny_triangle_trace.SWRT.mesh";
  triangleInput.mesh = bunny;
  triangleInput.prebuilt = triangleBuild;

  BLASManager::BuildInput rbvhInput{};
  rbvhInput.sourcePath = "tests/stanford_bunny_rbvh_trace.mesh";
  rbvhInput.fallbackPath = "tests/stanford_bunny_rbvh_trace.SWRBVH.mesh";
  rbvhInput.mesh = bunny;
  rbvhInput.rbvhBuild = rbvhBuild;

  {
    rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
    BLASManager manager(&renderGraph, blasTestSettingsForMesh(bunny, nullptr, &rbvhBuild));
    manager.syncFromCPU({rbvhInput});
    const uint32_t handle = manager.findHandleBySourcePath(rbvhInput.sourcePath);
    BLASManager::GPUBuildDescriptor descriptor{};
    expectTrue(manager.tryGetGPUBuildDescriptor(handle, &descriptor), "RBVH bunny BLAS descriptor lookup should succeed");
    expectTrue(descriptor.gpuCWBVHNodeCount <= static_cast<uint32_t>(rbvhBuild.cardBuild.canonicalNodes.size()), "RBVH packed CWBVH should collapse canonical card BVH nodes into wider packed nodes");

    const auto packedNodes =
        readBufferVector<BLASManager::CWBVHNodeGPU>(renderGraph, manager.getGPUCWBVHNodesBuffer(), descriptor.gpuCWBVHNodeCount, static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(BLASManager::CWBVHNodeGPU));
    uint32_t leafSlotTotal = 0u;
    uint32_t maxLeafSlotsInNode = 0u;
    for (const BLASManager::CWBVHNodeGPU &node : packedNodes)
    {
      const uint32_t leafCount = packedLeafSlotCount(node);
      expectTrue(leafCount <= 8u, "RBVH packed CWBVH node should expose at most eight card leaves");
      leafSlotTotal += leafCount;
      maxLeafSlotsInNode = std::max(maxLeafSlotsInNode, leafCount);
    }
    expectEqual(leafSlotTotal, static_cast<uint32_t>(rbvhBuild.cards.size()), "RBVH packed CWBVH should expose exactly one leaf per card");
    expectTrue(maxLeafSlotsInNode > 1u || rbvhBuild.cards.size() <= 1u, "RBVH packed CWBVH should allow multiple card leaves in one packed node");
  }

  const math::Vec3f boundsMin(bunny.boundsMin[0], bunny.boundsMin[1], bunny.boundsMin[2]);
  const math::Vec3f boundsMax(bunny.boundsMax[0], bunny.boundsMax[1], bunny.boundsMax[2]);
  const float meshDiagonal = std::max((boundsMax - boundsMin).length(), 1.0f);
  const float cardPointTolerance = meshDiagonal * 0.015f;
  const float approximateTraceTolerance = meshDiagonal * 0.25f;

  const std::vector<RBVHTraceKernelRay> candidates = makeRandomRbvhBunnyRays(rbvhBuild, bunny, 192u);
  expectTrue(candidates.size() >= 32u, "RBVH bunny test should generate enough covered random card rays");

  uint32_t comparedRayCount = 0u;
  uint32_t rbvhHitCount = 0u;
  for (const RBVHTraceKernelRay &ray : candidates)
  {
    if (comparedRayCount >= 32u)
      break;

    float cpuTriangleT = 0.0f;
    if (!findNearestTriangleHitT(bunny, ray.origin, ray.direction, &cpuTriangleT))
      continue;

    const math::Vec3f cpuTrianglePoint = ray.origin + ray.direction * cpuTriangleT;
    if ((cpuTrianglePoint - ray.cardPoint).length() > cardPointTolerance)
      continue;

    const SWRTRayTraceResult triangleResult = runSingleRayTraceForMesh(rhi, triangleInput, ray.origin, ray.direction, math::Vec3f(0.0f, 0.0f, 0.0f), false, false, SWRTPass::PrimitiveMode::Triangle);
    expectEqual(triangleResult.hit.instanceId != UINT32_MAX, true, "Triangle CWBVH bunny comparison ray should hit");

    const SWRTRayTraceResult rbvhResult = runSingleRayTraceForMesh(rhi, rbvhInput, ray.origin, ray.direction, math::Vec3f(0.0f, 0.0f, 0.0f), false, false, SWRTPass::PrimitiveMode::RBVHCard);
    if (rbvhResult.hit.instanceId != UINT32_MAX)
    {
      ++rbvhHitCount;
    }
    ++comparedRayCount;
  }

  expectEqual(comparedRayCount, 32u, "RBVH trace kernel should compare 32 random Stanford Bunny rays");
  expectTrue(rbvhHitCount >= 16u, "RBVH trace kernel should hit a representative subset of approximate Stanford Bunny rays");
}

void testRbvhTraceKernelHitsSingleCard(rendering::RHI &rhi)
{
  const virtualgeometry::FallbackMeshData mesh = makeFrontFacingTriangleMesh(0.0f);
  softwarebvh::RBVHBuildOptions rbvhOptions{};
  rbvhOptions.minCardResolution = 16u;
  rbvhOptions.maxCardResolution = 16u;
  rbvhOptions.maxAtlasSize = 64u;
  const softwarebvh::RBVHBuildResult rbvhBuild = softwarebvh::RBVHBuilder::build(mesh, rbvhOptions);
  expectEqual(static_cast<uint32_t>(rbvhBuild.cards.size()), 1u, "Single triangle RBVH should produce one card");

  BLASManager::BuildInput rbvhInput{};
  rbvhInput.sourcePath = "tests/single_triangle_rbvh_trace.mesh";
  rbvhInput.fallbackPath = "tests/single_triangle_rbvh_trace.SWRBVH.mesh";
  rbvhInput.mesh = mesh;
  rbvhInput.rbvhBuild = rbvhBuild;

  const SWRTRayTraceResult rbvhResult =
      runSingleRayTraceForMesh(rhi, rbvhInput, math::Vec3f(0.0f, 0.0f, 1.0f), math::Vec3f(0.0f, 0.0f, -1.0f), math::Vec3f(0.0f, 0.0f, 0.0f), false, false, SWRTPass::PrimitiveMode::RBVHCard);

  expectEqual(rbvhResult.hit.instanceId != UINT32_MAX, true, "RBVH single-card trace should hit");
  expectEqual(rbvhResult.hit.instanceId, 42u, "RBVH single-card trace should report scene instance id");
  expectEqual(rbvhResult.hit.triangleId, 0u, "RBVH single-card trace should report card primitive id");
}

void testRbvhBlasInstanceSourceKeysBuildDistinctHandles(rendering::RHI &rhi)
{
  const virtualgeometry::FallbackMeshData mesh = makeFrontFacingTriangleMesh(0.0f);
  softwarebvh::RBVHBuildOptions rbvhOptions{};
  rbvhOptions.minCardResolution = 16u;
  rbvhOptions.maxCardResolution = 16u;
  rbvhOptions.maxAtlasSize = 64u;
  const softwarebvh::RBVHBuildResult rbvhBuild = softwarebvh::RBVHBuilder::build(mesh, rbvhOptions);
  expectEqual(static_cast<uint32_t>(rbvhBuild.cards.size()), 1u, "Per-instance RBVH test should produce one card");

  BLASManager::BuildInput first{};
  first.sourcePath = "tests/instance_rbvh.vg::shape0::sceneInstance10";
  first.fallbackPath = "tests/instance_rbvh.lod0.fallback.mesh";
  first.mesh = mesh;
  first.rbvhBuild = rbvhBuild;

  BLASManager::BuildInput second = first;
  second.sourcePath = "tests/instance_rbvh.vg::shape0::sceneInstance11";

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  BLASManager manager(&renderGraph, blasTestSettingsForMesh(mesh, nullptr, &rbvhBuild));
  const BLASManager::BuildSummary summary = manager.syncFromCPU({first, second});
  expectEqual(summary.blasCount, 2u, "Per-instance RBVH source keys should produce one BLAS per scene instance");

  const uint32_t firstHandle = manager.findHandleBySourcePath(first.sourcePath);
  const uint32_t secondHandle = manager.findHandleBySourcePath(second.sourcePath);
  expectTrue(firstHandle != BLASManager::INVALID_BLAS_HANDLE, "First per-instance RBVH handle should exist");
  expectTrue(secondHandle != BLASManager::INVALID_BLAS_HANDLE, "Second per-instance RBVH handle should exist");
  expectTrue(firstHandle != secondHandle, "Per-instance RBVH handles should be distinct");

  BLASManager::GPUBuildDescriptor firstDescriptor{};
  BLASManager::GPUBuildDescriptor secondDescriptor{};
  expectTrue(manager.tryGetGPUBuildDescriptor(firstHandle, &firstDescriptor), "First per-instance RBVH descriptor should exist");
  expectTrue(manager.tryGetGPUBuildDescriptor(secondHandle, &secondDescriptor), "Second per-instance RBVH descriptor should exist");
  expectEqual(firstDescriptor.primitiveCount, static_cast<uint32_t>(rbvhBuild.cards.size()), "First per-instance RBVH primitive count");
  expectEqual(secondDescriptor.primitiveCount, static_cast<uint32_t>(rbvhBuild.cards.size()), "Second per-instance RBVH primitive count");
  expectTrue(firstDescriptor.gpuCWBVHNodeOffset != secondDescriptor.gpuCWBVHNodeOffset, "Per-instance RBVH BLASes should own distinct packed CWBVH ranges");
}

void testSwrtFallbackMeshLoadAndTrace(rendering::RHI &rhi)
{
  const virtualgeometry::FallbackMeshData sourceMesh = loadObjFallbackMesh(teapotAssetPath());
  const softwarebvh::SWRTFallbackMeshData built = softwarebvh::SWRTFallbackMeshFile::build(sourceMesh);
  compareBounds(built.build.localBounds, meshBounds(sourceMesh), "SWRT fallback build local bounds");

  const fs::path tempPath = fs::temp_directory_path() / "SoftwareBVHRuntimeTests_teapot.SWRTFallback.mesh";
  expectTrue(softwarebvh::SWRTFallbackMeshFile::save(tempPath.string(), built), "Saving SWRT fallback mesh should succeed");

  softwarebvh::SWRTFallbackMeshData loaded;
  expectTrue(softwarebvh::SWRTFallbackMeshFile::load(tempPath.string(), loaded), "Loading SWRT fallback mesh should succeed");
  std::error_code error;
  fs::remove(tempPath, error);

  compareBounds(meshBounds(loaded.fallbackMesh), meshBounds(sourceMesh), "Loaded SWRT fallback mesh bounds");
  compareBounds(loaded.build.localBounds, built.build.localBounds, "Loaded SWRT fallback build bounds");
  expectEqual(loaded.build.primitiveCount, built.build.primitiveCount, "Loaded SWRT fallback primitive count");
  const std::vector<TriangleRayTarget> rayTargets = selectExteriorTraceTargetTriangles(loaded.fallbackMesh, 3u);
  expectTrue(rayTargets.size() >= 3u, "SWRT fallback mesh should provide at least three visible exterior triangles");

  BLASManager::BuildInput blasInput{};
  blasInput.sourcePath = "tests/raytrace_teapot.mesh";
  blasInput.fallbackPath = tempPath.string();
  blasInput.mesh = loaded.fallbackMesh;
  blasInput.prebuilt = loaded.build;

  for (uint32_t targetIndex = 0u; targetIndex < 3u; ++targetIndex)
  {
    const TriangleRayTarget &rayTarget = rayTargets[targetIndex];
    const math::Vec3f cameraPosition = rayTarget.cameraPosition;
    const math::Vec3f cameraForward = (rayTarget.centroid - rayTarget.cameraPosition).normalize();

    const SWRTRayTraceResult result = runSingleRayTraceForMesh(rhi, blasInput, cameraPosition, cameraForward, math::Vec3f(0.0f, 0.0f, 0.0f));

    expectEqual(result.hit.instanceId != UINT32_MAX, true, "SWRT fallback trace output should be valid");
    expectEqual(result.hit.triangleId, rayTarget.primitiveIndex, "SWRT fallback trace should hit the targeted primitive");
    expectEqual(result.hit.instanceId, 42u, "SWRT fallback trace should record scene instance id");
  }
}

// One BVH4 TLAS over a terrain-page cut, at the scale the 16 km fixture
// actually streams.
//
// This is the measurement that justifies replacing the Tree64 clipmap
// directory. That directory is a uniform grid, so an instance costs one
// reference per cell it touches: the shipping 16 km cut reports 677970
// references for 930 instances, 729 apiece, because one cell size has to serve
// pages spanning 12.8 m to 13 km. A BVH stores each instance once. The leaf
// count assertion below is that difference stated as a test.
void testTlasOverTerrainPageCut(rendering::RHI &rhi)
{
  using voxel::svraster::SparseVoxelTree64DynamicInstanceSource;

  // Page edges follow the real cut: fine near the camera, doubling with
  // distance out to the render range. Placement is a pure translation, which is
  // how every terrain page is built.
  constexpr uint32_t pageCount = 930u;
  std::vector<SparseVoxelTree64DynamicInstanceSource> sources;
  sources.reserve(pageCount);
  double x = 0.0;
  double z = 0.0;
  for (uint32_t index = 0u; index < pageCount; ++index)
  {
    const uint32_t spatialLOD = index / 96u;
    const double edge = 12.8 * static_cast<double>(1u << spatialLOD);
    x += edge;
    if (x > 32000.0)
    {
      x = 0.0;
      z += edge;
    }
    SparseVoxelTree64DynamicInstanceSource source{};
    source.stableInstanceId = index;
    source.modelId = index;
    source.terrainGeneration = 1u;
    source.worldToModel = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      static_cast<float>(-x), 0.0f, static_cast<float>(-z), 1.0f,
    };
    source.worldBounds.minimum = {
      static_cast<float>(x), 0.0f, static_cast<float>(z)};
    source.worldBounds.maximum = {
      static_cast<float>(x + edge), 400.0f, static_cast<float>(z + edge)};
    sources.push_back(source);
  }

  const auto instances = voxel::svraster::sparseVoxelTree64TLASInstances(sources);
  expectEqual(static_cast<uint32_t>(instances.size()), pageCount,
      "terrain cut should convert every page to a TLAS instance");

  // Tree64 stores world-to-model; the TLAS wants object-to-world. Getting this
  // backwards builds a TLAS that traverses cleanly and intersects nothing, so
  // it is round-tripped rather than eyeballed.
  for (uint32_t index = 0u; index < pageCount; index += 97u)
  {
    const auto &input = instances[index];
    const auto &bounds = sources[index].worldBounds;
    expectTrue(
        std::abs(input.transform.data[12] - bounds.minimum[0]) < 1.0e-3f &&
            std::abs(input.transform.data[14] - bounds.minimum[2]) < 1.0e-3f,
        "object-to-world translation should place model origin at the page minimum");
    expectEqual(input.blasHandle, sources[index].modelId,
        "BLAS handle should address the Tree64 model");
    expectEqual(input.flags, TLASManager::INSTANCE_FLAG_DYNAMIC,
        "streamed terrain pages should be dynamic instances");
  }

  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = 1u});
  TLASManager::Settings settings{};
  settings.maxInstances = pageCount + 64u;
  settings.frameLocalBuffers = false;
  TLASManager manager(&renderGraph, settings);

  const TLASManager::BuildSummary summary =
      manager.rebuild(instances, static_cast<uint32_t>(instances.size()));
  expectEqual(summary.instanceCount, pageCount, "TLAS should carry every page");
  expectTrue(summary.rebuilt, "a first build over the cut should rebuild");
  expectTrue(summary.rootNodeIndex != TLASManager::INVALID_NODE,
      "TLAS build should produce a root");

  // The point of the exercise: one leaf per instance, no replication term.
  expectEqual(manager.getLeafInstanceCount(), pageCount,
      "a BVH stores each page once, unlike the grid directory's 729 references per instance");
  std::cout << "  terrain cut TLAS: " << pageCount << " pages, "
            << summary.nodeCount << " canonical nodes, "
            << manager.getBvh4NodeCount() << " bvh4 nodes, "
            << manager.getLeafInstanceCount() << " leaves\n";

  // Streaming churn: a slice of pages is republished each frame as the camera
  // moves, which is what the refit path exists for.
  auto moved = instances;
  std::vector<uint32_t> dirty;
  for (uint32_t index = 0u; index < pageCount; index += 32u)
  {
    moved[index].transform.data[13] += 1.0f;
    moved[index].worldBounds.minPoint[1] += 1.0f;
    moved[index].worldBounds.maxPoint[1] += 1.0f;
    dirty.push_back(index);
  }
  const TLASManager::BuildSummary refitSummary = manager.refit(moved, dirty);
  expectEqual(refitSummary.instanceCount, pageCount,
      "refit should retain every page");
  expectTrue(refitSummary.refitted || refitSummary.rebuilt || refitSummary.partialRebuilt,
      "a moved slice should refit, partially rebuild, or rebuild");
  std::cout << "  terrain cut refit: " << dirty.size() << " moved pages, refitted="
            << (refitSummary.refitted ? 1 : 0) << " partial="
            << (refitSummary.partialRebuilt ? 1 : 0) << " rebuilt="
            << (refitSummary.rebuilt ? 1 : 0) << "\n";
}

template <typename TestFn> void runTest(const char *name, rendering::RHI &rhi, TestFn test)
{
  if (!gTestFilter.empty() && std::string(name).find(gTestFilter) == std::string::npos)
    return;
  std::cout << "[RUN] " << name << "\n";
  test(rhi);
  std::cout << "[PASS] " << name << "\n";
}

template <typename TestFn> void runCpuTest(const char *name, TestFn test)
{
  if (!gTestFilter.empty() && std::string(name).find(gTestFilter) == std::string::npos)
    return;
  std::cout << "[RUN] " << name << "\n";
  test();
  std::cout << "[PASS] " << name << "\n";
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 1)
    gTestFilter = argv[1];

  os::Logger::start(100);

  runCpuTest("SwrtFallbackRtFilePreservesAllInstancesAndTriangles", testSwrtFallbackRtFilePreservesAllInstancesAndTriangles);
  runCpuTest("SponzaSwrtFallbackRtFilePreservesAllInstancesAndTriangles", testSponzaSwrtFallbackRtFilePreservesAllInstancesAndTriangles);

  rendering::DeviceRequiredLimits limits{0u, 0u, 0u};
  rendering::DeviceFeatures features = rendering::DeviceFeatures::DeviceFeatures_Compute;
  std::unique_ptr<vulkan::VulkanRHI> rhi;
  try
  {
    rhi = std::make_unique<vulkan::VulkanRHI>(vulkan::Vulkan_1_2, limits, features, std::vector<std::string>{});
    std::vector<VkSurfaceKHR> surfaces;
    rhi->init(surfaces);
  }
  catch (const std::exception &gpuError)
  {
    if (!isExpectedGpuBackendUnavailable(gpuError))
      throw;

    os::Logger::warningf("[Test] Software BVH runtime tests skipped: %s", gpuError.what());
    os::Logger::shutdown();
    std::cout << "SoftwareBVHRuntimeTests passed (GPU backend skipped)\n";
    return 0;
  }

  runTest("BlasCpuBuildAndBuffers", *rhi, testBlasCpuBuildAndBuffers);
  runTest("SwrtFallbackCollectionBuildsDistinctBlases", *rhi, testSwrtFallbackCollectionBuildsDistinctBlases);
  runTest("SoftwareBlasSceneBridgeBuildsAsyncFromFallbackMesh", *rhi, testSoftwareBlasSceneBridgeBuildsAsyncFromFallbackMesh);
  runTest("BlasRebuildClearsTriangleTransforms", *rhi, testBlasRebuildClearsTriangleTransforms);
  runTest("BlasGpuBuildMatchesCpuPackedCwbvh", *rhi, testBlasGpuBuildMatchesCpuPackedCwbvh);
  runTest("BlasGpuBuildHandlesLeafBeforeInternalChild", *rhi, testBlasGpuBuildHandlesLeafBeforeInternalChild);
  runTest("GpuBuiltCwbvhTraceAndBounds", *rhi, testGpuBuiltCwbvhTraceAndBounds);
  runTest("TlasRebuildAndRefit", *rhi, testTlasRebuildAndRefit);
  runTest("TlasRefitSurvivesInputReorder", *rhi, testTlasRefitSurvivesInputReorder);
  runTest("TlasPackedBoundsEncloseHierarchy", *rhi, testTlasPackedBoundsEncloseHierarchy);
  runTest("TlasPartialSubtreeRebuild", *rhi, testTlasPartialSubtreeRebuild);
  runTest("SponzaTlasBuildIndices", *rhi, testSponzaTlasBuildIndices);
  runTest("TlasGpuBuildHandlesLeafBeforeInternalChild", *rhi, testTlasGpuBuildHandlesLeafBeforeInternalChild);
  runTest("TlasGpuRebuildAndRefitPasses", *rhi, testTlasGpuRebuildAndRefitPasses);
  runTest("SoftwareRayTraceSingleRayHitAndMiss", *rhi, testSoftwareRayTraceSingleRayHitAndMiss);
  runTest("SoftwareRayTraceExternalRaySorting", *rhi, testSoftwareRayTraceExternalRaySorting);
  runTest("SoftwareRayTraceStreamExternalRaysMatchLocalTrace", *rhi, testSoftwareRayTraceStreamExternalRaysMatchLocalTrace);
  runTest("SoftwareRayTraceStreamDirectQueueProcessesAllMultiInstanceRays", *rhi, testSoftwareRayTraceStreamDirectQueueProcessesAllMultiInstanceRays);
  runTest("SoftwareRayTraceStreamSponzaMatchesLocalTrace", *rhi, testSoftwareRayTraceStreamSponzaMatchesLocalTrace);
  runTest("SoftwareRayTraceStreamSponzaDirectQueueMatchesLocalTrace", *rhi, testSoftwareRayTraceStreamSponzaDirectQueueMatchesLocalTrace);
  runTest("SoftwareRayTraceStreamSponzaMissOnlyMatchesLocalTrace", *rhi, testSoftwareRayTraceStreamSponzaMissOnlyMatchesLocalTrace);
  runTest("SoftwareRayTraceSponzaQuantizedTriangleTargets", *rhi, testSoftwareRayTraceSponzaQuantizedTriangleTargets);
  runTest("SoftwareRayTraceMultiInstanceTlas", *rhi, testSoftwareRayTraceMultiInstanceTlas);
  runTest("SoftwareRayTraceWorldCwbvhSurvivesInstanceUpdateAndGraphRecompile", *rhi, testSoftwareRayTraceWorldCwbvhSurvivesInstanceUpdateAndGraphRecompile);
  runTest("SoftwareRayTraceWorldCwbvhReportsOriginalTriangleIds", *rhi, testSoftwareRayTraceWorldCwbvhReportsOriginalTriangleIds);
  runTest("SoftwareRayTraceBvh4Tlas", *rhi, testSoftwareRayTraceBvh4Tlas);
  runTest("SoftwareRayTraceTlasTraceBenchmark", *rhi, testSoftwareRayTraceTlasTraceBenchmark);
  runTest("RbvhTraceKernelHitsSingleCard", *rhi, testRbvhTraceKernelHitsSingleCard);
  runTest("RbvhBlasInstanceSourceKeysBuildDistinctHandles", *rhi, testRbvhBlasInstanceSourceKeysBuildDistinctHandles);
  runTest("RbvhTraceKernelMatchesTriangleCwbvhBunnyRays", *rhi, testRbvhTraceKernelMatchesTriangleCwbvhBunnyRays);
  runTest("SwrtFallbackMeshLoadAndTrace", *rhi, testSwrtFallbackMeshLoadAndTrace);
  runTest("TlasOverTerrainPageCut", *rhi, testTlasOverTerrainPageCut);

  os::Logger::shutdown();
  std::cout << "SoftwareBVHRuntimeTests passed\n";
  return 0;
}
