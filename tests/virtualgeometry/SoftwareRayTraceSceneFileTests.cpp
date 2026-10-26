#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "softwarebvh/SoftwareRayTraceSceneFile.hpp"

namespace
{

namespace fs = std::filesystem;

bool nearEqual(float left, float right)
{
  return std::fabs(left - right) <= 1.0e-5f;
}

virtualgeometry::FallbackMeshData makeQuadMesh()
{
  virtualgeometry::FallbackMeshData mesh{};
  mesh.vertices.resize(4u);
  mesh.indices = {0u, 1u, 2u, 2u, 1u, 3u};
  mesh.sections = {virtualgeometry::FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(mesh.indices.size()), .materialIndex = 0u}};
  mesh.materialFiles = {"materials/quad.vmat"};
  mesh.boundsMin[0] = 0.0f;
  mesh.boundsMin[1] = 0.0f;
  mesh.boundsMin[2] = 0.0f;
  mesh.boundsMax[0] = 1.0f;
  mesh.boundsMax[1] = 1.0f;
  mesh.boundsMax[2] = 0.0f;

  const float positions[4][3] = {
      {0.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
      {1.0f, 1.0f, 0.0f},
  };
  const float uvs[4][2] = {
      {0.0f, 0.0f},
      {1.0f, 0.0f},
      {0.0f, 1.0f},
      {1.0f, 1.0f},
  };

  for (uint32_t vertexIndex = 0u; vertexIndex < 4u; ++vertexIndex)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      mesh.vertices[vertexIndex].position[axis] = positions[vertexIndex][axis];
    mesh.vertices[vertexIndex].normal[2] = 1.0f;
    mesh.vertices[vertexIndex].uv[0] = uvs[vertexIndex][0];
    mesh.vertices[vertexIndex].uv[1] = uvs[vertexIndex][1];
    mesh.vertices[vertexIndex].boneWeights[0] = 1.0f;
    mesh.vertices[vertexIndex].surfaceBaseColorOpacity[0] = 0.25f + 0.1f * static_cast<float>(vertexIndex);
    mesh.vertices[vertexIndex].surfaceBaseColorOpacity[1] = 0.5f;
    mesh.vertices[vertexIndex].surfaceBaseColorOpacity[2] = 0.75f;
    mesh.vertices[vertexIndex].surfaceBaseColorOpacity[3] = 1.0f;
    mesh.vertices[vertexIndex].surfaceNormalRoughness[0] = 0.0f;
    mesh.vertices[vertexIndex].surfaceNormalRoughness[1] = 0.0f;
    mesh.vertices[vertexIndex].surfaceNormalRoughness[2] = 1.0f;
    mesh.vertices[vertexIndex].surfaceNormalRoughness[3] = 0.35f + 0.05f * static_cast<float>(vertexIndex);
    mesh.vertices[vertexIndex].surfaceEmissiveMetallic[0] = 0.05f * static_cast<float>(vertexIndex);
    mesh.vertices[vertexIndex].surfaceEmissiveMetallic[3] = 0.2f;
    mesh.vertices[vertexIndex].surfaceFlags[0] = virtualgeometry::FallbackMeshSurfaceFlag_BakedVertexShading;
  }

  mesh.triangleSurfaceHints.resize(mesh.indices.size() / 3u);
  for (uint32_t triangleIndex = 0u; triangleIndex < static_cast<uint32_t>(mesh.triangleSurfaceHints.size()); ++triangleIndex)
  {
    virtualgeometry::FallbackMeshTriangleSurfaceHint &hint = mesh.triangleSurfaceHints[triangleIndex];
    hint.uvCircleCenter[0] = triangleIndex == 0u ? 0.33333334f : 0.66666669f;
    hint.uvCircleCenter[1] = triangleIndex == 0u ? 0.33333334f : 0.66666669f;
    hint.uvCircleRadius = 0.75f;
    hint.textureMipLevel = 10.0f;
    hint.materialIndex = 0u;
  }

  return mesh;
}

void compareMesh(const virtualgeometry::FallbackMeshData &expected, const virtualgeometry::FallbackMeshData &actual)
{
  assert(actual.vertices.size() == expected.vertices.size());
  assert(actual.indices == expected.indices);
  assert(actual.sections.size() == expected.sections.size());
  assert(actual.materialFiles == expected.materialFiles);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    assert(nearEqual(actual.boundsMin[axis], expected.boundsMin[axis]));
    assert(nearEqual(actual.boundsMax[axis], expected.boundsMax[axis]));
  }
  for (size_t vertexIndex = 0u; vertexIndex < expected.vertices.size(); ++vertexIndex)
  {
    assert(nearEqual(actual.vertices[vertexIndex].uv[0], expected.vertices[vertexIndex].uv[0]));
    assert(nearEqual(actual.vertices[vertexIndex].uv[1], expected.vertices[vertexIndex].uv[1]));
    for (uint32_t channel = 0u; channel < 4u; ++channel)
    {
      assert(nearEqual(actual.vertices[vertexIndex].surfaceBaseColorOpacity[channel], expected.vertices[vertexIndex].surfaceBaseColorOpacity[channel]));
      assert(nearEqual(actual.vertices[vertexIndex].surfaceNormalRoughness[channel], expected.vertices[vertexIndex].surfaceNormalRoughness[channel]));
      assert(nearEqual(actual.vertices[vertexIndex].surfaceEmissiveMetallic[channel], expected.vertices[vertexIndex].surfaceEmissiveMetallic[channel]));
      assert(actual.vertices[vertexIndex].surfaceFlags[channel] == expected.vertices[vertexIndex].surfaceFlags[channel]);
    }
  }
  assert(actual.triangleSurfaceHints.size() == expected.triangleSurfaceHints.size());
  for (size_t hintIndex = 0u; hintIndex < expected.triangleSurfaceHints.size(); ++hintIndex)
  {
    assert(nearEqual(actual.triangleSurfaceHints[hintIndex].uvCircleCenter[0], expected.triangleSurfaceHints[hintIndex].uvCircleCenter[0]));
    assert(nearEqual(actual.triangleSurfaceHints[hintIndex].uvCircleCenter[1], expected.triangleSurfaceHints[hintIndex].uvCircleCenter[1]));
    assert(nearEqual(actual.triangleSurfaceHints[hintIndex].uvCircleRadius, expected.triangleSurfaceHints[hintIndex].uvCircleRadius));
    assert(nearEqual(actual.triangleSurfaceHints[hintIndex].textureMipLevel, expected.triangleSurfaceHints[hintIndex].textureMipLevel));
    assert(actual.triangleSurfaceHints[hintIndex].materialIndex == expected.triangleSurfaceHints[hintIndex].materialIndex);
  }
}

void compareBuild(const softwarebvh::CWBVHBuilder::BuildResult &expected, const softwarebvh::CWBVHBuilder::BuildResult &actual)
{
  assert(actual.primitiveCount == expected.primitiveCount);
  assert(actual.rootNodeIndex == expected.rootNodeIndex);
  assert(actual.primitiveRemap == expected.primitiveRemap);
  assert(actual.triangleTransforms.size() == expected.triangleTransforms.size());
  assert(actual.canonicalNodes.size() == expected.canonicalNodes.size());
  assert(actual.bvh8Nodes.size() == expected.bvh8Nodes.size());
  assert(actual.bvh8LeafRemap == expected.bvh8LeafRemap);
  for (size_t nodeIndex = 0u; nodeIndex < expected.bvh8Nodes.size(); ++nodeIndex)
  {
    const softwarebvh::CWBVHBuilder::BVH8Node &expectedNode = expected.bvh8Nodes[nodeIndex];
    const softwarebvh::CWBVHBuilder::BVH8Node &actualNode = actual.bvh8Nodes[nodeIndex];
    assert(actualNode.cwbvhOutputIndex == expectedNode.cwbvhOutputIndex);
    assert(actualNode.internalChildBase == expectedNode.internalChildBase);
    assert(actualNode.leafBase == expectedNode.leafBase);
    assert(actualNode.validChildMask == expectedNode.validChildMask);
    assert(actualNode.internalChildMask == expectedNode.internalChildMask);
    assert(actualNode.leafChildMask == expectedNode.leafChildMask);
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      assert(actualNode.childIndex[childSlot] == expectedNode.childIndex[childSlot]);
      assert(actualNode.childMeta[childSlot] == expectedNode.childMeta[childSlot]);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        assert(nearEqual(actualNode.childMin[childSlot][axis], expectedNode.childMin[childSlot][axis]));
        assert(nearEqual(actualNode.childMax[childSlot][axis], expectedNode.childMax[childSlot][axis]));
      }
    }
  }
}

uint32_t packedChildInfo(const softwarebvh::CWBVHBuilder::PackedCWBVHNode &node, uint32_t childSlot)
{
  return (node.info[childSlot >> 2u] >> ((childSlot & 3u) * 8u)) & 0xffu;
}

void validateCompressedBVH8(const softwarebvh::CWBVHBuilder::BuildResult &build)
{
  const softwarebvh::CWBVHBuilder::CompressionResult compressed =
      softwarebvh::CWBVHBuilder::compressBVH8(build.bvh8Nodes, build.bvh8LeafRemap);
  assert(!compressed.nodes.empty());
  assert(compressed.nodes.size() == build.bvh8Nodes.size());
  assert(compressed.leafRemap == build.bvh8LeafRemap);

  std::vector<uint8_t> visitedNodes(compressed.nodes.size(), 0u);
  std::vector<uint8_t> seenPrimitives(build.primitiveCount, 0u);
  std::vector<uint32_t> stack{0u};
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    assert(nodeIndex < compressed.nodes.size());
    assert(visitedNodes[nodeIndex] == 0u);
    visitedNodes[nodeIndex] = 1u;

    const softwarebvh::CWBVHBuilder::PackedCWBVHNode &node = compressed.nodes[nodeIndex];
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
        const uint32_t childNodeIndex = node.ntbi[0] + internalOrdinal;
        assert(childNodeIndex < compressed.nodes.size());
        stack.push_back(childNodeIndex);
        ++internalOrdinal;
      }
      else
      {
        const uint32_t leafIndex = node.ntbi[1] + localOffset;
        assert(leafIndex < compressed.leafRemap.size());
        const uint32_t primitiveIndex = compressed.leafRemap[leafIndex];
        assert(primitiveIndex < build.primitiveCount);
        seenPrimitives[primitiveIndex] = 1u;
      }
    }
    assert(childCount > 0u);
  }

  for (const uint8_t visited : visitedNodes)
    assert(visited == 1u);
  for (const uint8_t seen : seenPrimitives)
    assert(seen == 1u);
}

} // namespace

int main()
{
  const fs::path tempPath = fs::temp_directory_path() / "SoftwareRayTraceSceneFileTests.softwareraytrace";
  const virtualgeometry::FallbackMeshData mesh = makeQuadMesh();
  virtualgeometry::FallbackMeshData lod1Mesh = mesh;
  lod1Mesh.lodLevel = 1u;

  std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> entries;
  entries.push_back(softwarebvh::SoftwareRayTraceSceneFile::buildInstance(mesh, 10u, 100u, 0u, "asset.virtualgeometry::shape0"));
  entries.push_back(softwarebvh::SoftwareRayTraceSceneFile::buildInstance(mesh, 11u, 101u, 1u, "asset.virtualgeometry::shape1"));
  entries.push_back(softwarebvh::SoftwareRayTraceSceneFile::buildInstance(lod1Mesh, 12u, 102u, 0u, "asset.virtualgeometry::lod1::shape0"));
  entries[1u].flags = 17u;
  for (const softwarebvh::SoftwareRayTraceSceneInstanceData &entry : entries)
  {
    assert(!entry.build.bvh8Nodes.empty());
    assert(entry.build.bvh8LeafRemap.size() == entry.build.primitiveCount);
    validateCompressedBVH8(entry.build);
  }

  assert(softwarebvh::SoftwareRayTraceSceneFile::save(tempPath.string(), entries));

  softwarebvh::SoftwareRayTraceSceneFileHeader header{};
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadHeader(tempPath.string(), header));
  assert(header.version == softwarebvh::SOFTWARE_RAYTRACE_SCENE_FILE_VERSION);
  assert(header.headerSize > 20u);
  assert(header.entries.size() == entries.size());
  assert(header.entries[0u].sceneInstanceId == 100u);
  assert(header.entries[1u].sceneInstanceId == 101u);
  assert(header.entries[2u].lodLevel == 1u);
  assert(header.entries[2u].shapeIndex == 0u);
  assert(header.entries[1u].flags == 17u);
  assert(header.entries[0u].compressedSize > 0u);
  assert(header.entries[0u].uncompressedSize > header.entries[0u].compressedSize || header.entries[0u].uncompressedSize > 0u);
  assert(header.entries[0u].cwbvhPayloadOffset >= header.headerSize);
  assert(header.entries[0u].cwbvhCompressedSize > 0u);
  assert(header.entries[0u].cwbvhUncompressedSize > 0u);
  assert(header.entries[0u].fallbackPayloadOffset == header.entries[0u].cwbvhPayloadOffset + header.entries[0u].cwbvhCompressedSize);
  assert(header.entries[0u].fallbackCompressedSize > 0u);
  assert(header.entries[0u].fallbackUncompressedSize > 0u);

  std::vector<softwarebvh::SoftwareRayTraceSceneInstanceData> loadedEntries;
  assert(softwarebvh::SoftwareRayTraceSceneFile::load(tempPath.string(), loadedEntries));
  assert(loadedEntries.size() == entries.size());
  for (size_t entryIndex = 0u; entryIndex < entries.size(); ++entryIndex)
  {
    assert(loadedEntries[entryIndex].objectInstanceId == entries[entryIndex].objectInstanceId);
    assert(loadedEntries[entryIndex].sceneInstanceId == entries[entryIndex].sceneInstanceId);
    assert(loadedEntries[entryIndex].shapeIndex == entries[entryIndex].shapeIndex);
    assert(loadedEntries[entryIndex].lodLevel == entries[entryIndex].lodLevel);
    assert(loadedEntries[entryIndex].flags == entries[entryIndex].flags);
    assert(loadedEntries[entryIndex].sourcePath == entries[entryIndex].sourcePath);
    compareMesh(entries[entryIndex].fallbackMesh, loadedEntries[entryIndex].fallbackMesh);
    compareBuild(entries[entryIndex].build, loadedEntries[entryIndex].build);
  }

  softwarebvh::SoftwareRayTraceSceneInstanceData loadedEntry{};
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntry(tempPath.string(), 1u, loadedEntry));
  assert(loadedEntry.sceneInstanceId == 101u);
  assert(loadedEntry.shapeIndex == 1u);
  compareMesh(entries[1u].fallbackMesh, loadedEntry.fallbackMesh);
  compareBuild(entries[1u].build, loadedEntry.build);

  softwarebvh::SoftwareRayTraceSceneInstanceData loadedBuildOnly{};
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryCWBVH(tempPath.string(), 1u, loadedBuildOnly));
  assert(loadedBuildOnly.sceneInstanceId == 101u);
  assert(loadedBuildOnly.shapeIndex == 1u);
  assert(loadedBuildOnly.fallbackMesh.vertices.empty());
  assert(loadedBuildOnly.fallbackMesh.indices.empty());
  compareBuild(entries[1u].build, loadedBuildOnly.build);

  softwarebvh::SoftwareRayTraceSceneInstanceData loadedFallbackOnly{};
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryFallbackMesh(tempPath.string(), 1u, loadedFallbackOnly));
  assert(loadedFallbackOnly.sceneInstanceId == 101u);
  assert(loadedFallbackOnly.shapeIndex == 1u);
  assert(loadedFallbackOnly.build.bvh8Nodes.empty());
  assert(loadedFallbackOnly.build.primitiveCount == 0u);
  compareMesh(entries[1u].fallbackMesh, loadedFallbackOnly.fallbackMesh);

  uint32_t entryIndex = UINT32_MAX;
  assert(softwarebvh::SoftwareRayTraceSceneFile::findEntryIndex(header, 1u, 0u, entryIndex));
  assert(entryIndex == 2u);
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntry(tempPath.string(), 1u, 0u, loadedEntry));
  assert(loadedEntry.sceneInstanceId == 102u);
  assert(loadedEntry.lodLevel == 1u);
  assert(loadedEntry.shapeIndex == 0u);
  compareMesh(entries[2u].fallbackMesh, loadedEntry.fallbackMesh);
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryCWBVH(tempPath.string(), 1u, 0u, loadedBuildOnly));
  compareBuild(entries[2u].build, loadedBuildOnly.build);
  assert(loadedBuildOnly.fallbackMesh.vertices.empty());
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryFallbackMesh(tempPath.string(), 1u, 0u, loadedFallbackOnly));
  compareMesh(entries[2u].fallbackMesh, loadedFallbackOnly.fallbackMesh);
  assert(loadedFallbackOnly.build.bvh8Nodes.empty());

  assert(softwarebvh::SoftwareRayTraceSceneFile::findEntryIndexForShape(header, 1u, entryIndex));
  assert(entryIndex == 1u);
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryForShape(tempPath.string(), 1u, loadedEntry));
  assert(loadedEntry.sceneInstanceId == 101u);
  assert(loadedEntry.shapeIndex == 1u);
  compareMesh(entries[1u].fallbackMesh, loadedEntry.fallbackMesh);
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryCWBVHForShape(tempPath.string(), 1u, loadedBuildOnly));
  compareBuild(entries[1u].build, loadedBuildOnly.build);
  assert(softwarebvh::SoftwareRayTraceSceneFile::loadEntryFallbackMeshForShape(tempPath.string(), 1u, loadedFallbackOnly));
  compareMesh(entries[1u].fallbackMesh, loadedFallbackOnly.fallbackMesh);

  fs::remove(tempPath);
  return 0;
}
