#include "VirtualGeometryBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metis.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

namespace virtualgeometry
{

namespace
{

struct GLTFAssetData
{
  std::vector<Vertex> vertices;
  std::vector<ShapeBuildInput> shapes;
  rendering::animation::Skeleton skeleton;
};

static math::Mat4f buildNodeLocalTransform(const tinygltf::Node &node)
{
  if (node.matrix.size() == 16u)
  {
    math::Mat4f transform{};
    for (size_t i = 0; i < 16u; ++i)
      transform.data[i] = static_cast<float>(node.matrix[i]);
    return transform;
  }

  math::Vec3f translation(0.0f, 0.0f, 0.0f);
  if (node.translation.size() == 3u)
    translation = math::Vec3f(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));

  math::Quatf rotation = math::Quatf::identity();
  if (node.rotation.size() == 4u)
    rotation = math::Quatf(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3]));

  math::Vec3f scale(1.0f, 1.0f, 1.0f);
  if (node.scale.size() == 3u)
    scale = math::Vec3f(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));

  return math::Mat4f::translate(translation) * math::Mat4f::fromQuaternion(rotation) * math::Mat4f::scale(scale);
}

static math::Vec3f transformPosition(const math::Mat4f &transform, const math::Vec3f &position)
{
  const math::Vec4f p(position.x(), position.y(), position.z(), 1.0f);
  const math::Vec4f transformed = transform * p;
  return math::Vec3f(transformed[0], transformed[1], transformed[2]);
}

static math::Vec3f transformDirection(const math::Mat4f &transform, const math::Vec3f &direction)
{
  const math::Vec4f d(direction.x(), direction.y(), direction.z(), 0.0f);
  const math::Vec4f transformed = transform * d;
  return math::Vec3f(transformed[0], transformed[1], transformed[2]).normalize();
}

static double readNumericComponent(const unsigned char *data, int componentType, bool normalized)
{
  switch (componentType)
  {
  case TINYGLTF_COMPONENT_TYPE_BYTE:
  {
    int8_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return std::max(static_cast<double>(value) / 127.0, -1.0);
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
  {
    uint8_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return static_cast<double>(value) / 255.0;
  }
  case TINYGLTF_COMPONENT_TYPE_SHORT:
  {
    int16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return std::max(static_cast<double>(value) / 32767.0, -1.0);
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
  {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if (!normalized)
      return static_cast<double>(value);
    return static_cast<double>(value) / 65535.0;
  }
  case TINYGLTF_COMPONENT_TYPE_INT:
  {
    int32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<double>(value);
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
  {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<double>(value);
  }
  case TINYGLTF_COMPONENT_TYPE_FLOAT:
  {
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<double>(value);
  }
  case TINYGLTF_COMPONENT_TYPE_DOUBLE:
  {
    double value = 0.0;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }
  default:
    return 0.0;
  }
}

static uint32_t readUnsignedComponent(const unsigned char *data, int componentType)
{
  switch (componentType)
  {
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
  {
    uint8_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
  {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }
  case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
  {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }
  case TINYGLTF_COMPONENT_TYPE_BYTE:
  {
    int8_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<uint32_t>(std::max<int32_t>(value, 0));
  }
  case TINYGLTF_COMPONENT_TYPE_SHORT:
  {
    int16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<uint32_t>(std::max<int32_t>(value, 0));
  }
  case TINYGLTF_COMPONENT_TYPE_INT:
  {
    int32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<uint32_t>(std::max<int32_t>(value, 0));
  }
  case TINYGLTF_COMPONENT_TYPE_FLOAT:
  {
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    return static_cast<uint32_t>(std::max(value, 0.0f));
  }
  default:
    return 0u;
  }
}

static bool readAccessorFloatComponents(const tinygltf::Model &model, int accessorIndex, int expectedComponents, std::vector<float> &out)
{
  if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
    return false;

  const tinygltf::Accessor &accessor = model.accessors[accessorIndex];
  if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type)) != expectedComponents)
    return false;
  if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    return false;

  const tinygltf::BufferView &bufferView = model.bufferViews[accessor.bufferView];
  if (bufferView.buffer < 0 || bufferView.buffer >= static_cast<int>(model.buffers.size()))
    return false;

  const tinygltf::Buffer &buffer = model.buffers[bufferView.buffer];
  const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
  const int stride = accessor.ByteStride(bufferView);
  if (componentSize <= 0 || stride <= 0)
    return false;

  const size_t accessorOffset = bufferView.byteOffset + accessor.byteOffset;
  if (accessorOffset >= buffer.data.size())
    return false;

  out.resize(accessor.count * static_cast<size_t>(expectedComponents));
  for (size_t element = 0; element < accessor.count; ++element)
  {
    const unsigned char *elementData = buffer.data.data() + accessorOffset + element * static_cast<size_t>(stride);
    for (int component = 0; component < expectedComponents; ++component)
    {
      const unsigned char *componentData = elementData + component * componentSize;
      out[element * static_cast<size_t>(expectedComponents) + static_cast<size_t>(component)] = static_cast<float>(readNumericComponent(componentData, accessor.componentType, accessor.normalized));
    }
  }

  return true;
}

static bool readAccessorUIntComponents(const tinygltf::Model &model, int accessorIndex, int expectedComponents, std::vector<uint32_t> &out)
{
  if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
    return false;

  const tinygltf::Accessor &accessor = model.accessors[accessorIndex];
  if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type)) != expectedComponents)
    return false;
  if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    return false;

  const tinygltf::BufferView &bufferView = model.bufferViews[accessor.bufferView];
  if (bufferView.buffer < 0 || bufferView.buffer >= static_cast<int>(model.buffers.size()))
    return false;

  const tinygltf::Buffer &buffer = model.buffers[bufferView.buffer];
  const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
  const int stride = accessor.ByteStride(bufferView);
  if (componentSize <= 0 || stride <= 0)
    return false;

  const size_t accessorOffset = bufferView.byteOffset + accessor.byteOffset;
  if (accessorOffset >= buffer.data.size())
    return false;

  out.resize(accessor.count * static_cast<size_t>(expectedComponents));
  for (size_t element = 0; element < accessor.count; ++element)
  {
    const unsigned char *elementData = buffer.data.data() + accessorOffset + element * static_cast<size_t>(stride);
    for (int component = 0; component < expectedComponents; ++component)
    {
      const unsigned char *componentData = elementData + component * componentSize;
      out[element * static_cast<size_t>(expectedComponents) + static_cast<size_t>(component)] = readUnsignedComponent(componentData, accessor.componentType);
    }
  }

  return true;
}

[[maybe_unused]] static uint32_t vertexDominantBoneIndex(const Vertex &vertex)
{
  float bestWeight = -1.0f;
  uint32_t bestBone = UINT32_MAX;
  for (const BoneWeight &boneWeight : vertex.boneWeights)
  {
    if (boneWeight.weight > bestWeight)
    {
      bestWeight = boneWeight.weight;
      bestBone = boneWeight.boneIndex;
    }
  }
  return bestBone;
}

[[maybe_unused]] static int sharedBoneWeight(const Vertex &a, const Vertex &b)
{
  float totalWeight = 0.0f;
  for (const BoneWeight &wa : a.boneWeights)
    for (const BoneWeight &wb : b.boneWeights)
      if (wa.boneIndex == wb.boneIndex)
        totalWeight += std::min(wa.weight, wb.weight);

  return std::max(1, static_cast<int>(std::lround(totalWeight * 1000.0f)));
}

[[maybe_unused]] static std::vector<MeshPart> buildMeshPartsFromBoneAffinity(const std::vector<Vertex> &vertices, const Shape &shape)
{
  std::set<uint32_t> usedBones;
  std::vector<uint32_t> usedVertices;
  usedVertices.reserve(shape.indices.size());

  std::unordered_map<uint32_t, uint32_t> globalToLocal;
  for (uint32_t globalIndex : shape.indices)
  {
    if (globalToLocal.find(globalIndex) == globalToLocal.end())
    {
      globalToLocal[globalIndex] = static_cast<uint32_t>(usedVertices.size());
      usedVertices.push_back(globalIndex);
      for (const BoneWeight &boneWeight : vertices[globalIndex].boneWeights)
        if (boneWeight.weight > 0.0f)
          usedBones.insert(boneWeight.boneIndex);
    }
  }

  if (usedVertices.empty())
    return {};

  if (usedBones.empty())
    return {MeshPart{shape, UINT32_MAX}};

  if (usedBones.size() == 1u)
    return {MeshPart{shape, *usedBones.begin()}};

  const int vertexCount = static_cast<int>(usedVertices.size());
  std::vector<int> part(vertexCount, 0);
  std::unordered_map<uint32_t, uint32_t> boneToPart;
  std::vector<uint32_t> sortedBones(usedBones.begin(), usedBones.end());
  for (uint32_t i = 0u; i < sortedBones.size(); ++i)
    boneToPart[sortedBones[i]] = i;

  std::unordered_map<uint64_t, int> edgeWeights;
  auto addEdge = [&](uint32_t a, uint32_t b, int weight)
  {
    if (a == b || weight <= 0)
      return;
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    const uint64_t key = (static_cast<uint64_t>(lo) << 32u) | hi;
    edgeWeights[key] += weight;
  };

  for (size_t triangle = 0; triangle + 2u < shape.indices.size(); triangle += 3u)
  {
    const uint32_t l0 = globalToLocal[shape.indices[triangle + 0u]];
    const uint32_t l1 = globalToLocal[shape.indices[triangle + 1u]];
    const uint32_t l2 = globalToLocal[shape.indices[triangle + 2u]];
    addEdge(l0, l1, sharedBoneWeight(vertices[usedVertices[l0]], vertices[usedVertices[l1]]));
    addEdge(l1, l2, sharedBoneWeight(vertices[usedVertices[l1]], vertices[usedVertices[l2]]));
    addEdge(l2, l0, sharedBoneWeight(vertices[usedVertices[l2]], vertices[usedVertices[l0]]));
  }

  if (!edgeWeights.empty())
  {
    std::vector<std::vector<std::pair<int, int>>> neighbors(static_cast<size_t>(vertexCount));
    for (const auto &entry : edgeWeights)
    {
      const int a = static_cast<int>(entry.first >> 32u);
      const int b = static_cast<int>(entry.first & 0xFFFFFFFFu);
      neighbors[static_cast<size_t>(a)].push_back({b, entry.second});
      neighbors[static_cast<size_t>(b)].push_back({a, entry.second});
    }

    std::vector<int> xadj(static_cast<size_t>(vertexCount) + 1u, 0);
    std::vector<int> adjncy;
    std::vector<int> adjwgt;
    for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
      for (const auto &neighbor : neighbors[static_cast<size_t>(vertexIndex)])
      {
        adjncy.push_back(neighbor.first);
        adjwgt.push_back(neighbor.second);
      }
      xadj[static_cast<size_t>(vertexIndex) + 1u] = static_cast<int>(adjncy.size());
    }

    int options[METIS_NOPTIONS];
    METIS_SetDefaultOptions(options);
    options[METIS_OPTION_SEED] = 42;
    options[METIS_OPTION_UFACTOR] = 200;

    int nvtxs = vertexCount;
    int ncon = 1;
    int edgecut = 0;
    int nparts = std::min<int>(static_cast<int>(usedBones.size()), vertexCount);
    if (nparts > 1)
    {
      const int result = METIS_PartGraphRecursive(
          &nvtxs, &ncon, xadj.data(), adjncy.empty() ? nullptr : adjncy.data(), nullptr, nullptr, adjwgt.empty() ? nullptr : adjwgt.data(), &nparts, nullptr, nullptr, options, &edgecut, part.data());
      if (result != METIS_OK)
        throw std::runtime_error("METIS bone-affinity partition failed");
    }
  }
  else
  {
    for (int localVertex = 0; localVertex < vertexCount; ++localVertex)
    {
      const uint32_t dominantBone = vertexDominantBoneIndex(vertices[usedVertices[static_cast<size_t>(localVertex)]]);
      part[static_cast<size_t>(localVertex)] = dominantBone == UINT32_MAX ? 0 : static_cast<int>(boneToPart[dominantBone]);
    }
  }

  std::vector<std::unordered_map<uint32_t, float>> partBoneWeights(sortedBones.size());
  for (int localVertex = 0; localVertex < vertexCount; ++localVertex)
  {
    const Vertex &vertex = vertices[usedVertices[static_cast<size_t>(localVertex)]];
    auto &weights = partBoneWeights[static_cast<size_t>(part[static_cast<size_t>(localVertex)])];
    for (const BoneWeight &boneWeight : vertex.boneWeights)
      weights[boneWeight.boneIndex] += boneWeight.weight;
  }

  std::vector<uint32_t> partDominantBones(partBoneWeights.size(), UINT32_MAX);
  for (size_t partIndex = 0; partIndex < partBoneWeights.size(); ++partIndex)
  {
    float bestWeight = -1.0f;
    for (const auto &entry : partBoneWeights[partIndex])
    {
      if (entry.second > bestWeight)
      {
        bestWeight = entry.second;
        partDominantBones[partIndex] = entry.first;
      }
    }
  }

  std::vector<MeshPart> meshParts(partBoneWeights.size());
  for (size_t partIndex = 0; partIndex < meshParts.size(); ++partIndex)
    meshParts[partIndex].dominantBoneIndex = partDominantBones[partIndex];

  for (size_t triangle = 0; triangle + 2u < shape.indices.size(); triangle += 3u)
  {
    const uint32_t g0 = shape.indices[triangle + 0u];
    const uint32_t g1 = shape.indices[triangle + 1u];
    const uint32_t g2 = shape.indices[triangle + 2u];
    const uint32_t l0 = globalToLocal[g0];
    const uint32_t l1 = globalToLocal[g1];
    const uint32_t l2 = globalToLocal[g2];

    std::unordered_map<int, int> partCounts;
    partCounts[part[static_cast<size_t>(l0)]]++;
    partCounts[part[static_cast<size_t>(l1)]]++;
    partCounts[part[static_cast<size_t>(l2)]]++;

    int bestPart = part[static_cast<size_t>(l0)];
    int bestCount = -1;
    float bestScore = -1.0f;

    for (const auto &entry : partCounts)
    {
      const uint32_t dominantBone = partDominantBones[static_cast<size_t>(entry.first)];
      float score = 0.0f;
      if (dominantBone != UINT32_MAX)
      {
        for (const BoneWeight &boneWeight : vertices[g0].boneWeights)
          if (boneWeight.boneIndex == dominantBone)
            score += boneWeight.weight;
        for (const BoneWeight &boneWeight : vertices[g1].boneWeights)
          if (boneWeight.boneIndex == dominantBone)
            score += boneWeight.weight;
        for (const BoneWeight &boneWeight : vertices[g2].boneWeights)
          if (boneWeight.boneIndex == dominantBone)
            score += boneWeight.weight;
      }

      if (entry.second > bestCount || (entry.second == bestCount && score > bestScore))
      {
        bestPart = entry.first;
        bestCount = entry.second;
        bestScore = score;
      }
    }

    MeshPart &meshPart = meshParts[static_cast<size_t>(bestPart)];
    meshPart.shape.indices.push_back(g0);
    meshPart.shape.indices.push_back(g1);
    meshPart.shape.indices.push_back(g2);
  }

  std::vector<MeshPart> compacted;
  compacted.reserve(meshParts.size());
  for (MeshPart &meshPart : meshParts)
    if (!meshPart.shape.indices.empty())
      compacted.push_back(std::move(meshPart));
  return compacted;
}

static void computeNormals(std::vector<Vertex> &vertices, const Shape &shape)
{
  for (uint32_t globalIndex : shape.indices)
  {
    vertices[globalIndex].norm[0] = 0.0f;
    vertices[globalIndex].norm[1] = 0.0f;
    vertices[globalIndex].norm[2] = 0.0f;
  }

  for (size_t triangle = 0; triangle + 2u < shape.indices.size(); triangle += 3u)
  {
    Vertex &v0 = vertices[shape.indices[triangle + 0u]];
    Vertex &v1 = vertices[shape.indices[triangle + 1u]];
    Vertex &v2 = vertices[shape.indices[triangle + 2u]];

    const float x1 = v1.pos[0] - v0.pos[0];
    const float y1 = v1.pos[1] - v0.pos[1];
    const float z1 = v1.pos[2] - v0.pos[2];
    const float x2 = v2.pos[0] - v0.pos[0];
    const float y2 = v2.pos[1] - v0.pos[1];
    const float z2 = v2.pos[2] - v0.pos[2];

    const float nx = y1 * z2 - z1 * y2;
    const float ny = z1 * x2 - x1 * z2;
    const float nz = x1 * y2 - y1 * x2;

    v0.norm[0] += nx;
    v0.norm[1] += ny;
    v0.norm[2] += nz;
    v1.norm[0] += nx;
    v1.norm[1] += ny;
    v1.norm[2] += nz;
    v2.norm[0] += nx;
    v2.norm[1] += ny;
    v2.norm[2] += nz;
  }

  std::unordered_map<uint32_t, bool> normalized;
  for (uint32_t globalIndex : shape.indices)
  {
    if (normalized[globalIndex])
      continue;
    normalized[globalIndex] = true;

    math::Vec3f normal(vertices[globalIndex].norm[0], vertices[globalIndex].norm[1], vertices[globalIndex].norm[2]);
    normal = normal.normalize();
    vertices[globalIndex].norm[0] = normal.x();
    vertices[globalIndex].norm[1] = normal.y();
    vertices[globalIndex].norm[2] = normal.z();
  }
}

static void collectNodeWorldTransforms(const tinygltf::Model &model, int nodeIndex, const math::Mat4f &parentTransform, std::vector<math::Mat4f> &worldTransforms)
{
  if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
    return;

  const tinygltf::Node &node = model.nodes[static_cast<size_t>(nodeIndex)];
  const math::Mat4f worldTransform = parentTransform * buildNodeLocalTransform(node);
  worldTransforms[static_cast<size_t>(nodeIndex)] = worldTransform;

  for (int childNode : node.children)
    collectNodeWorldTransforms(model, childNode, worldTransform, worldTransforms);
}

static void buildSkeletonFromSkin(const tinygltf::Model &model, int skinIndex, int skinnedNodeIndex, const std::vector<math::Mat4f> &worldTransforms, rendering::animation::Skeleton &outSkeleton)
{
  if (skinIndex < 0 || skinIndex >= static_cast<int>(model.skins.size()))
    return;

  const tinygltf::Skin &skin = model.skins[static_cast<size_t>(skinIndex)];
  outSkeleton.clear();
  if (skinnedNodeIndex >= 0 && skinnedNodeIndex < static_cast<int>(worldTransforms.size()))
    outSkeleton.setDefaultTransform(worldTransforms[static_cast<size_t>(skinnedNodeIndex)]);

  std::vector<float> inverseBindMatrices;
  if (skin.inverseBindMatrices >= 0)
    readAccessorFloatComponents(model, skin.inverseBindMatrices, 16, inverseBindMatrices);

  std::unordered_map<int, uint32_t> nodeToBoneIndex;
  for (size_t jointIndex = 0; jointIndex < skin.joints.size(); ++jointIndex)
    nodeToBoneIndex[skin.joints[jointIndex]] = static_cast<uint32_t>(jointIndex);

  for (size_t jointIndex = 0; jointIndex < skin.joints.size(); ++jointIndex)
  {
    const int nodeIndex = skin.joints[jointIndex];
    const tinygltf::Node &jointNode = model.nodes[static_cast<size_t>(nodeIndex)];

    rendering::animation::Skeleton::Bone bone;
    bone.name = jointNode.name.empty() ? ("bone_" + std::to_string(jointIndex)) : jointNode.name;
    bone.defaultLocalTransform = buildNodeLocalTransform(jointNode);

    if (inverseBindMatrices.size() >= (jointIndex + 1u) * 16u)
      for (size_t element = 0; element < 16u; ++element)
        bone.inverseBindMatrix.data[element] = inverseBindMatrices[jointIndex * 16u + element];

    bone.parentIndex = -1;
    for (const auto &entry : nodeToBoneIndex)
    {
      const tinygltf::Node &candidate = model.nodes[static_cast<size_t>(entry.first)];
      for (int childNode : candidate.children)
        if (childNode == nodeIndex)
        {
          bone.parentIndex = static_cast<int32_t>(entry.second);
          break;
        }
      if (bone.parentIndex >= 0)
        break;
    }

    outSkeleton.addBone(bone);
  }
}

static GLTFAssetData loadGLTFAsset(const std::string &path)
{
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string warnings;
  std::string errors;

  const bool isBinary = path.size() >= 4u && path.substr(path.size() - 4u) == ".glb";
  const bool loaded = isBinary ? loader.LoadBinaryFromFile(&model, &errors, &warnings, path) : loader.LoadASCIIFromFile(&model, &errors, &warnings, path);
  if (!warnings.empty())
    std::printf("%s\n", warnings.c_str());
  if (!loaded)
    throw std::runtime_error("Failed to load glTF: " + path + (errors.empty() ? std::string() : " (" + errors + ")"));

  GLTFAssetData asset;
  std::vector<math::Mat4f> worldTransforms(model.nodes.size(), math::Mat4f::identity());

  if (!model.scenes.empty())
  {
    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
    const tinygltf::Scene &scene = model.scenes[static_cast<size_t>(sceneIndex)];
    for (int nodeIndex : scene.nodes)
      collectNodeWorldTransforms(model, nodeIndex, math::Mat4f::identity(), worldTransforms);
  }
  else
  {
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
      collectNodeWorldTransforms(model, static_cast<int>(nodeIndex), math::Mat4f::identity(), worldTransforms);
  }

  int firstSkinIndex = -1;
  int firstSkinnedNodeIndex = -1;

  for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
  {
    const tinygltf::Node &node = model.nodes[nodeIndex];
    if (node.mesh < 0 || node.mesh >= static_cast<int>(model.meshes.size()))
      continue;

    if (firstSkinIndex < 0 && node.skin >= 0)
    {
      firstSkinIndex = node.skin;
      firstSkinnedNodeIndex = static_cast<int>(nodeIndex);
      buildSkeletonFromSkin(model, firstSkinIndex, firstSkinnedNodeIndex, worldTransforms, asset.skeleton);
    }

    const math::Mat4f nodeTransform = nodeIndex < worldTransforms.size() ? worldTransforms[nodeIndex] : math::Mat4f::identity();
    const tinygltf::Mesh &mesh = model.meshes[static_cast<size_t>(node.mesh)];

    for (const tinygltf::Primitive &primitive : mesh.primitives)
    {
      const int mode = primitive.mode == -1 ? TINYGLTF_MODE_TRIANGLES : primitive.mode;
      if (mode != TINYGLTF_MODE_TRIANGLES)
        continue;

      auto positionIt = primitive.attributes.find("POSITION");
      if (positionIt == primitive.attributes.end())
        continue;

      std::vector<float> positions;
      if (!readAccessorFloatComponents(model, positionIt->second, 3, positions))
        continue;
      const size_t vertexCount = positions.size() / 3u;

      std::vector<float> normals;
      const auto normalIt = primitive.attributes.find("NORMAL");
      const bool hasNormals = normalIt != primitive.attributes.end() && readAccessorFloatComponents(model, normalIt->second, 3, normals);

      std::vector<float> texcoords;
      const auto texcoordIt = primitive.attributes.find("TEXCOORD_0");
      const bool hasTexcoords = texcoordIt != primitive.attributes.end() && readAccessorFloatComponents(model, texcoordIt->second, 2, texcoords);

      std::vector<uint32_t> joints;
      const auto jointsIt = primitive.attributes.find("JOINTS_0");
      const bool hasJoints = jointsIt != primitive.attributes.end() && readAccessorUIntComponents(model, jointsIt->second, 4, joints);

      std::vector<float> weights;
      const auto weightsIt = primitive.attributes.find("WEIGHTS_0");
      const bool hasWeights = weightsIt != primitive.attributes.end() && readAccessorFloatComponents(model, weightsIt->second, 4, weights);

      const uint32_t vertexBase = static_cast<uint32_t>(asset.vertices.size());
      asset.vertices.resize(asset.vertices.size() + vertexCount);

      for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
      {
        Vertex &vertex = asset.vertices[static_cast<size_t>(vertexBase) + vertexIndex];
        const math::Vec3f localPosition(positions[vertexIndex * 3u + 0u], positions[vertexIndex * 3u + 1u], positions[vertexIndex * 3u + 2u]);
        const math::Vec3f worldPosition = transformPosition(nodeTransform, localPosition);
        vertex.pos[0] = worldPosition.x();
        vertex.pos[1] = worldPosition.y();
        vertex.pos[2] = worldPosition.z();

        if (hasNormals)
        {
          const math::Vec3f localNormal(normals[vertexIndex * 3u + 0u], normals[vertexIndex * 3u + 1u], normals[vertexIndex * 3u + 2u]);
          const math::Vec3f worldNormal = transformDirection(nodeTransform, localNormal);
          vertex.norm[0] = worldNormal.x();
          vertex.norm[1] = worldNormal.y();
          vertex.norm[2] = worldNormal.z();
        }

        if (hasTexcoords)
        {
          vertex.uv[0] = texcoords[vertexIndex * 2u + 0u];
          vertex.uv[1] = texcoords[vertexIndex * 2u + 1u];
        }

        if (node.skin >= 0 && hasJoints && hasWeights)
        {
          float totalWeight = 0.0f;
          vertex.boneWeights.reserve(4u);
          for (size_t component = 0; component < 4u; ++component)
          {
            BoneWeight boneWeight;
            boneWeight.weight = weights[vertexIndex * 4u + component];
            boneWeight.boneIndex = joints[vertexIndex * 4u + component];
            vertex.boneWeights.push_back(boneWeight);
            totalWeight += std::max(0.0f, boneWeight.weight);
          }

          if (totalWeight > 0.0f)
            for (BoneWeight &boneWeight : vertex.boneWeights)
              boneWeight.weight = std::max(0.0f, boneWeight.weight) / totalWeight;
          else
            vertex.boneWeights.clear();
        }
      }

      Shape shape;
      if (primitive.indices >= 0)
      {
        std::vector<uint32_t> indices;
        if (!readAccessorUIntComponents(model, primitive.indices, 1, indices))
          continue;
        shape.indices.reserve(indices.size());
        for (uint32_t index : indices)
          shape.indices.push_back(vertexBase + index);
      }
      else
      {
        shape.indices.reserve(vertexCount);
        for (uint32_t index = 0u; index < static_cast<uint32_t>(vertexCount); ++index)
          shape.indices.push_back(vertexBase + index);
      }

      if (!hasNormals)
        computeNormals(asset.vertices, shape);

      ShapeBuildInput shapeInput;
      shapeInput.materialIndex = primitive.material >= 0 ? static_cast<uint32_t>(primitive.material) : 0u;
      shapeInput.skeleton = asset.skeleton.empty() ? nullptr : &asset.skeleton;
      shapeInput.meshParts.push_back(MeshPart{shape, UINT32_MAX});

      asset.shapes.push_back(std::move(shapeInput));
    }
  }

  std::vector<uint32_t> meshPartToBoneIndex;
  meshPartToBoneIndex.reserve(asset.skeleton.getBoneCount());
  for (uint32_t boneIndex = 0u; boneIndex < asset.skeleton.getBoneCount(); ++boneIndex)
    meshPartToBoneIndex.push_back(boneIndex);
  asset.skeleton.setMeshPartToBoneIndex(std::move(meshPartToBoneIndex));

  return asset;
}

} // namespace

VirtualGeometryBuildData VirtualGeometryBuilder::buildFromGLTFFile(const std::string &path, const VirtualGeometryBuildSettings &settings)
{
  GLTFAssetData asset = loadGLTFAsset(path);
  VirtualGeometryBuildData buildData = build(asset.vertices, asset.shapes, settings);
  buildData.skeleton = asset.skeleton;
  return buildData;
}

} // namespace virtualgeometry
