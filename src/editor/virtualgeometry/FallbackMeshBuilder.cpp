#include "FallbackMeshBuilder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>

#include "meshoptimizer.h"
#include "virtualgeometry/VirtualImageFile.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

namespace virtualgeometry
{

namespace
{

constexpr size_t kNormalAttributeCount = 3u;
constexpr size_t kUvAttributeCount = 2u;
constexpr size_t kBaseAttributeCount = kNormalAttributeCount + kUvAttributeCount;
constexpr size_t kMaxSimplifierInfluences = (32u - kBaseAttributeCount) / 2u;
constexpr float kNormalAttributeWeight = 0.75f;
constexpr float kUvAttributeWeight = 2.0f;
constexpr float kBoneIndexAttributeWeight = 4.0f;
constexpr float kBoneWeightAttributeWeight = 2.0f;
constexpr float kManySeamsRatio = 1.1f;
constexpr float kBoundsLockRelativeEpsilon = 1.0e-5f;
constexpr float kBoundsLockAbsoluteEpsilon = 1.0e-6f;
constexpr float kDefaultFallbackTextureSize = 1024.0f;
constexpr float kSharpFeatureNormalDotThreshold = 0.5f;

struct SectionGeometry
{
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  uint32_t materialIndex = 0u;
};

struct BakedMaterialSurface
{
  float baseColorOpacity[4] = {0.85f, 0.85f, 0.85f, 1.0f};
  float normalRoughness[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float emissiveMetallic[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t flags[4] = {FallbackMeshSurfaceFlag_BakedVertexShading, 0u, 0u, 0u};
  VirtualTextureSystem::PreparedImage baseColorTexture;
  VirtualTextureSystem::TextureSampling baseColorSampling{};
};

struct VertexSurfaceAccumulator
{
  float baseColorOpacity[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float weight = 0.0f;
};

template <typename Function>
void parallelFor(size_t count, Function &&function)
{
  if (count == 0u)
    return;

  const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
  const size_t workerCount = std::min<size_t>(count, hardwareThreads);
  if (workerCount <= 1u)
  {
    for (size_t index = 0u; index < count; ++index)
      function(index);
    return;
  }

  std::atomic<size_t> nextIndex{0u};
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex)
  {
    workers.emplace_back(
        [&]()
        {
          for (;;)
          {
            const size_t index = nextIndex.fetch_add(1u, std::memory_order_relaxed);
            if (index >= count)
              break;
            function(index);
          }
        });
  }

  for (std::thread &worker : workers)
    worker.join();
}

struct SimplifySectionOptions
{
  bool lockBorders = true;
  bool regularize = true;
  bool lockBoundsExtrema = true;
};

size_t getMaxBoneInfluenceCount(const std::vector<Vertex> &vertices)
{
  size_t maxInfluenceCount = 0u;
  for (const Vertex &vertex : vertices)
    maxInfluenceCount = std::max(maxInfluenceCount, vertex.boneWeights.size());
  return std::min(maxInfluenceCount, kMaxSimplifierInfluences);
}

uint32_t getMaxBoneIndex(const std::vector<Vertex> &vertices)
{
  uint32_t maxBoneIndex = 0u;
  for (const Vertex &vertex : vertices)
    for (const BoneWeight &boneWeight : vertex.boneWeights)
      maxBoneIndex = std::max(maxBoneIndex, boneWeight.boneIndex);
  return maxBoneIndex;
}

void buildSimplifierAttributes(
    const std::vector<Vertex> &vertices,
    std::vector<float> &attributes,
    std::vector<float> &attributeWeights)
{
  const size_t influenceCount = getMaxBoneInfluenceCount(vertices);
  const size_t attributeCount = kBaseAttributeCount + influenceCount * 2u;
  attributes.resize(vertices.size() * attributeCount, 0.0f);
  attributeWeights.assign(attributeCount, 0.0f);

  attributeWeights[0] = kNormalAttributeWeight;
  attributeWeights[1] = kNormalAttributeWeight;
  attributeWeights[2] = kNormalAttributeWeight;
  attributeWeights[3] = kUvAttributeWeight;
  attributeWeights[4] = kUvAttributeWeight;

  for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
  {
    attributeWeights[kBaseAttributeCount + influenceIndex * 2u + 0u] = kBoneIndexAttributeWeight;
    attributeWeights[kBaseAttributeCount + influenceIndex * 2u + 1u] = kBoneWeightAttributeWeight;
  }

  const uint32_t maxBoneIndex = getMaxBoneIndex(vertices);
  const float boneIndexScale = maxBoneIndex > 0u ? (1.0f / static_cast<float>(maxBoneIndex)) : 0.0f;

  for (size_t vertexIndex = 0u; vertexIndex < vertices.size(); ++vertexIndex)
  {
    const Vertex &vertex = vertices[vertexIndex];
    const size_t attributeBase = vertexIndex * attributeCount;
    attributes[attributeBase + 0u] = vertex.norm[0];
    attributes[attributeBase + 1u] = vertex.norm[1];
    attributes[attributeBase + 2u] = vertex.norm[2];
    attributes[attributeBase + 3u] = vertex.uv[0];
    attributes[attributeBase + 4u] = vertex.uv[1];

    for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
    {
      if (influenceIndex >= vertex.boneWeights.size())
        continue;

      const size_t influenceBase = attributeBase + kBaseAttributeCount + influenceIndex * 2u;
      attributes[influenceBase + 0u] = static_cast<float>(vertex.boneWeights[influenceIndex].boneIndex) * boneIndexScale;
      attributes[influenceBase + 1u] = vertex.boneWeights[influenceIndex].weight;
    }
  }
}

bool hasManySeams(const std::vector<Vertex> &vertices)
{
  if (vertices.empty())
    return false;

  std::vector<uint32_t> remap(vertices.size(), 0u);
  meshopt_generatePositionRemap(remap.data(), &vertices[0].pos[0], vertices.size(), sizeof(Vertex));

  uint32_t uniquePositionCount = 0u;
  for (uint32_t vertexIndex = 0u; vertexIndex < remap.size(); ++vertexIndex)
    uniquePositionCount = std::max(uniquePositionCount, remap[vertexIndex] + 1u);

  if (uniquePositionCount == 0u)
    return false;

  return static_cast<double>(vertices.size()) / static_cast<double>(uniquePositionCount) >= kManySeamsRatio;
}

uint64_t encodeUndirectedEdge(uint32_t a, uint32_t b)
{
  if (a > b)
    std::swap(a, b);
  return (static_cast<uint64_t>(a) << 32u) | static_cast<uint64_t>(b);
}

std::array<float, 3> normalizeDirection(float x, float y, float z)
{
  const float length = std::sqrt(x * x + y * y + z * z);
  if (length <= 0.0f)
    return {0.0f, 0.0f, 0.0f};
  const float invLength = 1.0f / length;
  return {x * invLength, y * invLength, z * invLength};
}

std::vector<std::array<float, 3>> buildSilhouetteDirections()
{
  std::vector<std::array<float, 3>> directions;
  directions.reserve(15u);
  directions.push_back(normalizeDirection(1.0f, 0.0f, 0.0f));
  directions.push_back(normalizeDirection(0.0f, 1.0f, 0.0f));
  directions.push_back(normalizeDirection(0.0f, 0.0f, 1.0f));

  for (int sx : {-1, 1})
    for (int sy : {-1, 1})
      directions.push_back(normalizeDirection(static_cast<float>(sx), static_cast<float>(sy), 0.0f));

  for (int sx : {-1, 1})
    for (int sz : {-1, 1})
      directions.push_back(normalizeDirection(static_cast<float>(sx), 0.0f, static_cast<float>(sz)));

  for (int sy : {-1, 1})
    for (int sz : {-1, 1})
      directions.push_back(normalizeDirection(0.0f, static_cast<float>(sy), static_cast<float>(sz)));

  return directions;
}

std::array<float, 3> computeTriangleNormal(
    const std::vector<Vertex> &vertices,
    uint32_t i0,
    uint32_t i1,
    uint32_t i2)
{
  if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
    return {0.0f, 0.0f, 0.0f};

  const Vertex &v0 = vertices[i0];
  const Vertex &v1 = vertices[i1];
  const Vertex &v2 = vertices[i2];
  const float e10[3] = {
      v1.pos[0] - v0.pos[0],
      v1.pos[1] - v0.pos[1],
      v1.pos[2] - v0.pos[2],
  };
  const float e20[3] = {
      v2.pos[0] - v0.pos[0],
      v2.pos[1] - v0.pos[1],
      v2.pos[2] - v0.pos[2],
  };

  return normalizeDirection(
      e10[1] * e20[2] - e10[2] * e20[1],
      e10[2] * e20[0] - e10[0] * e20[2],
      e10[0] * e20[1] - e10[1] * e20[0]);
}

std::vector<unsigned char> buildProxyVertexLocks(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
  std::vector<unsigned char> locks(vertices.size(), 0u);
  if (vertices.empty())
    return locks;

  std::vector<uint32_t> positionRemap(vertices.size(), 0u);
  meshopt_generatePositionRemap(positionRemap.data(), &vertices[0].pos[0], vertices.size(), sizeof(Vertex));

  uint32_t uniquePositionCount = 0u;
  for (uint32_t vertexIndex = 0u; vertexIndex < positionRemap.size(); ++vertexIndex)
    uniquePositionCount = std::max(uniquePositionCount, positionRemap[vertexIndex] + 1u);

  std::vector<std::vector<uint32_t>> positionVertices(uniquePositionCount);
  for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(positionRemap.size()); ++vertexIndex)
    positionVertices[positionRemap[vertexIndex]].push_back(vertexIndex);

  float boundsMin[3] = {vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2]};
  float boundsMax[3] = {vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2]};
  uint32_t minIndex[3] = {0u, 0u, 0u};
  uint32_t maxIndex[3] = {0u, 0u, 0u};
  for (const Vertex &vertex : vertices)
  {
    const uint32_t vertexIndex = static_cast<uint32_t>(&vertex - vertices.data());
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (vertex.pos[axis] < boundsMin[axis])
      {
        boundsMin[axis] = vertex.pos[axis];
        minIndex[axis] = vertexIndex;
      }
      if (vertex.pos[axis] > boundsMax[axis])
      {
        boundsMax[axis] = vertex.pos[axis];
        maxIndex[axis] = vertexIndex;
      }
    }
  }

  const auto lockVertex = [&locks](const uint32_t vertexIndex)
  {
    if (vertexIndex < locks.size())
      locks[vertexIndex] |= meshopt_SimplifyVertex_Lock;
  };

  const auto lockPosition = [&](const uint32_t vertexIndex)
  {
    if (vertexIndex >= positionRemap.size())
      return;

    const uint32_t positionIndex = positionRemap[vertexIndex];
    if (positionIndex >= positionVertices.size())
      return;

    for (const uint32_t duplicateVertexIndex : positionVertices[positionIndex])
      lockVertex(duplicateVertexIndex);
  };

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    lockPosition(minIndex[axis]);
    lockPosition(maxIndex[axis]);

    const float extent = boundsMax[axis] - boundsMin[axis];
    const float epsilon = std::max(kBoundsLockAbsoluteEpsilon, extent * kBoundsLockRelativeEpsilon);
    if (extent <= epsilon)
      continue;

    const uint32_t otherAxis0 = (axis + 1u) % 3u;
    const uint32_t otherAxis1 = (axis + 2u) % 3u;
    for (uint32_t side = 0u; side < 2u; ++side)
    {
      const float plane = side == 0u ? boundsMin[axis] : boundsMax[axis];
      for (int sign0 : {-1, 1})
      {
        for (int sign1 : {-1, 1})
        {
          uint32_t bestVertexIndex = side == 0u ? minIndex[axis] : maxIndex[axis];
          float bestScore = -std::numeric_limits<float>::max();
          for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(vertices.size()); ++vertexIndex)
          {
            const Vertex &vertex = vertices[vertexIndex];
            if (std::fabs(vertex.pos[axis] - plane) > epsilon)
              continue;

            const float score = static_cast<float>(sign0) * vertex.pos[otherAxis0] + static_cast<float>(sign1) * vertex.pos[otherAxis1];
            if (score > bestScore)
            {
              bestScore = score;
              bestVertexIndex = vertexIndex;
            }
          }
          lockPosition(bestVertexIndex);
        }
      }
    }
  }

  const std::vector<std::array<float, 3>> silhouetteDirections = buildSilhouetteDirections();
  for (const std::array<float, 3> &direction : silhouetteDirections)
  {
    uint32_t minVertexIndex = 0u;
    uint32_t maxVertexIndex = 0u;
    float minProjection = std::numeric_limits<float>::max();
    float maxProjection = -std::numeric_limits<float>::max();
    for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(vertices.size()); ++vertexIndex)
    {
      const Vertex &vertex = vertices[vertexIndex];
      const float projection =
          vertex.pos[0] * direction[0] +
          vertex.pos[1] * direction[1] +
          vertex.pos[2] * direction[2];
      if (projection < minProjection)
      {
        minProjection = projection;
        minVertexIndex = vertexIndex;
      }
      if (projection > maxProjection)
      {
        maxProjection = projection;
        maxVertexIndex = vertexIndex;
      }
    }
    lockPosition(minVertexIndex);
    lockPosition(maxVertexIndex);
  }

  struct EdgeRecord
  {
    uint64_t edge = 0u;
    uint32_t i0 = 0u;
    uint32_t i1 = 0u;
    std::array<float, 3> normal{};
  };

  std::vector<EdgeRecord> edges;
  edges.reserve(indices.size());
  for (size_t index = 0u; index + 2u < indices.size(); index += 3u)
  {
    const uint32_t i0 = indices[index + 0u];
    const uint32_t i1 = indices[index + 1u];
    const uint32_t i2 = indices[index + 2u];
    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
      continue;

    const std::array<float, 3> normal = computeTriangleNormal(vertices, i0, i1, i2);
    if (normal[0] == 0.0f && normal[1] == 0.0f && normal[2] == 0.0f)
      continue;

    const auto addEdge = [&](const uint32_t a, const uint32_t b)
    {
      edges.push_back(EdgeRecord{
          .edge = encodeUndirectedEdge(positionRemap[a], positionRemap[b]),
          .i0 = a,
          .i1 = b,
          .normal = normal,
      });
    };

    addEdge(i0, i1);
    addEdge(i1, i2);
    addEdge(i2, i0);
  }
  std::sort(
      edges.begin(),
      edges.end(),
      [](const EdgeRecord &left, const EdgeRecord &right)
      {
        return left.edge < right.edge;
      });

  std::vector<uint32_t> boundaryVertices;
  for (size_t edgeIndex = 0u; edgeIndex < edges.size();)
  {
    const uint64_t edge = edges[edgeIndex].edge;
    size_t nextEdgeIndex = edgeIndex + 1u;
    while (nextEdgeIndex < edges.size() && edges[nextEdgeIndex].edge == edge)
      ++nextEdgeIndex;

    if (nextEdgeIndex - edgeIndex == 1u)
    {
      boundaryVertices.push_back(edges[edgeIndex].i0);
      boundaryVertices.push_back(edges[edgeIndex].i1);
    }
    else
    {
      bool lockSharpEdge = nextEdgeIndex - edgeIndex > 2u;
      for (size_t left = edgeIndex; left < nextEdgeIndex && !lockSharpEdge; ++left)
      {
        for (size_t right = left + 1u; right < nextEdgeIndex; ++right)
        {
          const std::array<float, 3> &leftNormal = edges[left].normal;
          const std::array<float, 3> &rightNormal = edges[right].normal;
          const float normalDot =
              leftNormal[0] * rightNormal[0] +
              leftNormal[1] * rightNormal[1] +
              leftNormal[2] * rightNormal[2];
          if (normalDot < kSharpFeatureNormalDotThreshold)
          {
            lockSharpEdge = true;
            break;
          }
        }
      }

      if (lockSharpEdge)
      {
        for (size_t edgeRecordIndex = edgeIndex; edgeRecordIndex < nextEdgeIndex; ++edgeRecordIndex)
        {
          lockPosition(edges[edgeRecordIndex].i0);
          lockPosition(edges[edgeRecordIndex].i1);
        }
      }
    }
    edgeIndex = nextEdgeIndex;
  }
  std::sort(boundaryVertices.begin(), boundaryVertices.end());
  boundaryVertices.erase(std::unique(boundaryVertices.begin(), boundaryVertices.end()), boundaryVertices.end());

  if (!boundaryVertices.empty())
  {
    for (int signX : {-1, 1})
    {
      for (int signY : {-1, 1})
      {
        for (int signZ : {-1, 1})
        {
          uint32_t bestVertexIndex = boundaryVertices.front();
          float bestScore = -std::numeric_limits<float>::max();
          for (const uint32_t vertexIndex : boundaryVertices)
          {
            const Vertex &vertex = vertices[vertexIndex];
            const float score =
                static_cast<float>(signX) * vertex.pos[0] +
                static_cast<float>(signY) * vertex.pos[1] +
                static_cast<float>(signZ) * vertex.pos[2];
            if (score > bestScore)
            {
              bestScore = score;
              bestVertexIndex = vertexIndex;
            }
          }
          lockPosition(bestVertexIndex);
        }
      }
    }
  }

  return locks;
}

SectionGeometry buildSectionGeometry(
    const std::vector<Vertex> &globalVertices,
    const SourceShapeBuildInput &shape)
{
  SectionGeometry section;
  section.materialIndex = shape.materialIndex;

  std::vector<uint32_t> globalIndices;
  for (const MeshPart &meshPart : shape.meshParts)
    globalIndices.insert(globalIndices.end(), meshPart.shape.indices.begin(), meshPart.shape.indices.end());

  if (globalIndices.empty())
    return section;

  std::vector<uint32_t> uniqueGlobalIndices = globalIndices;
  std::sort(uniqueGlobalIndices.begin(), uniqueGlobalIndices.end());
  uniqueGlobalIndices.erase(std::unique(uniqueGlobalIndices.begin(), uniqueGlobalIndices.end()), uniqueGlobalIndices.end());

  section.vertices.resize(uniqueGlobalIndices.size());
  for (size_t vertexIndex = 0u; vertexIndex < uniqueGlobalIndices.size(); ++vertexIndex)
    section.vertices[vertexIndex] = globalVertices[uniqueGlobalIndices[vertexIndex]];

  section.indices.resize(globalIndices.size());
  for (size_t index = 0u; index < globalIndices.size(); ++index)
  {
    auto it = std::lower_bound(uniqueGlobalIndices.begin(), uniqueGlobalIndices.end(), globalIndices[index]);
    section.indices[index] = static_cast<uint32_t>(it - uniqueGlobalIndices.begin());
  }

  return section;
}

std::vector<SectionGeometry> buildSourceSections(const VirtualGeometrySourceAsset &asset)
{
  std::vector<SectionGeometry> sections;
  sections.reserve(asset.shapes.size());
  for (const SourceShapeBuildInput &shape : asset.shapes)
  {
    SectionGeometry section = buildSectionGeometry(asset.vertices, shape);
    if (!section.indices.empty())
      sections.push_back(std::move(section));
  }
  return sections;
}

std::vector<std::string> buildMaterialFileTable(const VirtualGeometrySourceAsset &asset)
{
  size_t materialCount = asset.materialFiles.size();
  for (const SourceShapeBuildInput &shape : asset.shapes)
    materialCount = std::max(materialCount, static_cast<size_t>(shape.materialIndex) + 1u);

  std::vector<std::string> materialFiles(materialCount);
  for (size_t materialIndex = 0u; materialIndex < asset.materialFiles.size() && materialIndex < materialFiles.size(); ++materialIndex)
    materialFiles[materialIndex] = asset.materialFiles[materialIndex];
  return materialFiles;
}

BakedMaterialSurface makeBakedMaterialSurface(const Material::Definition &definition)
{
  BakedMaterialSurface surface{};
  for (uint32_t channel = 0u; channel < 4u; ++channel)
    surface.baseColorOpacity[channel] = definition.baseColorFactor[channel];
  surface.baseColorOpacity[3] = std::clamp(definition.baseColorFactor[3] * definition.opacity, 0.0f, 1.0f);
  surface.normalRoughness[3] = std::clamp(definition.roughnessFactor, 0.0f, 1.0f);
  surface.emissiveMetallic[0] = std::max(0.0f, definition.emissiveFactor[0] * definition.emissiveStrength);
  surface.emissiveMetallic[1] = std::max(0.0f, definition.emissiveFactor[1] * definition.emissiveStrength);
  surface.emissiveMetallic[2] = std::max(0.0f, definition.emissiveFactor[2] * definition.emissiveStrength);
  surface.emissiveMetallic[3] = std::clamp(definition.metallicFactor, 0.0f, 1.0f);
  return surface;
}

std::string lowercaseAscii(std::string value)
{
  for (char &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool isBaseColorTextureName(const std::string &name)
{
  const std::string normalized = lowercaseAscii(name);
  return normalized == "basecolor" ||
         normalized == "base_color" ||
         normalized == "albedo" ||
         normalized == "diffuse" ||
         normalized == "diffusecolor" ||
         normalized == "diffuse_color";
}

uint32_t findBaseColorTextureSlot(const Material::Definition &definition)
{
  for (uint32_t textureIndex = 0u; textureIndex < definition.textureCount && textureIndex < MAX_TEXTURES_PER_MATERIAL; ++textureIndex)
  {
    if (isBaseColorTextureName(definition.textures[textureIndex].name))
      return textureIndex;
  }

  return definition.textureCount != 0u ? 0u : Material::InvalidTextureId;
}

bool loadMaterialTexture(
    const Material::Definition &definition,
    const std::filesystem::path &materialPath,
    uint32_t textureSlot,
    VirtualTextureSystem::PreparedImage &outImage)
{
  if (textureSlot >= definition.textureCount || textureSlot >= MAX_TEXTURES_PER_MATERIAL)
    return false;

  const std::string &storedImagePath = definition.textures[textureSlot].imageFilePath;
  if (storedImagePath.empty())
    return false;

  std::filesystem::path imagePath(storedImagePath);
  if (imagePath.is_relative())
    imagePath = materialPath.parent_path() / imagePath;

  return VirtualImageFile::load(imagePath.lexically_normal().string(), outImage);
}

float textureDimensionForMipHint(const BakedMaterialSurface &surface, uint32_t axis, const FallbackMeshBuilder::SurfaceBakeSettings &settings)
{
  if (!surface.baseColorTexture.mips.empty())
  {
    const VirtualTextureSystem::PreparedMipLevel &baseMip = surface.baseColorTexture.mips.front();
    return static_cast<float>(axis == 0u ? std::max(baseMip.width, 1u) : std::max(baseMip.height, 1u));
  }

  if (axis == 0u)
    return settings.fallbackTextureWidth > 0u ? static_cast<float>(settings.fallbackTextureWidth) : kDefaultFallbackTextureSize;
  return settings.fallbackTextureHeight > 0u ? static_cast<float>(settings.fallbackTextureHeight) : kDefaultFallbackTextureSize;
}

float computeOnePixelMipLevel(float uvRadius, const BakedMaterialSurface &surface, const FallbackMeshBuilder::SurfaceBakeSettings &settings)
{
  const float textureWidth = textureDimensionForMipHint(surface, 0u, settings);
  const float textureHeight = textureDimensionForMipHint(surface, 1u, settings);
  const float textureSize = std::max(textureWidth, textureHeight);
  const float diameterTexels = std::max(uvRadius * 2.0f * textureSize, 1.0f);
  return std::max(0.0f, std::log2(diameterTexels));
}

uint32_t clampTextureMipLevel(
    const BakedMaterialSurface &surface,
    float mipLevel)
{
  if (surface.baseColorTexture.mips.empty())
    return 0u;

  const uint32_t highestMip = static_cast<uint32_t>(surface.baseColorTexture.mips.size() - 1u);
  const uint32_t minMip = std::min(surface.baseColorSampling.minMip, highestMip);
  const uint32_t maxMip = std::min(std::max(surface.baseColorSampling.maxMip, minMip), highestMip);
  const float biasedMip = std::floor(std::max(0.0f, mipLevel + surface.baseColorSampling.mipBias));
  return static_cast<uint32_t>(std::clamp(biasedMip, static_cast<float>(minMip), static_cast<float>(maxMip)));
}

float applyTextureAddressMode(float coord, VirtualTextureSystem::TextureAddressMode mode)
{
  if (mode == VirtualTextureSystem::TextureAddressMode::ClampToEdge)
    return std::clamp(coord, 0.0f, 1.0f);
  return coord - std::floor(coord);
}

std::array<float, 4> unpackRGBA8(uint32_t texel)
{
  constexpr float inv255 = 1.0f / 255.0f;
  return {
      static_cast<float>(texel & 0xFFu) * inv255,
      static_cast<float>((texel >> 8u) & 0xFFu) * inv255,
      static_cast<float>((texel >> 16u) & 0xFFu) * inv255,
      static_cast<float>((texel >> 24u) & 0xFFu) * inv255};
}

std::array<float, 4> loadTextureMipTexel(
    const VirtualTextureSystem::PreparedMipLevel &mip,
    int32_t x,
    int32_t y)
{
  const uint32_t width = std::max(mip.width, 1u);
  const uint32_t height = std::max(mip.height, 1u);
  const uint32_t clampedX = static_cast<uint32_t>(std::clamp<int32_t>(x, 0, static_cast<int32_t>(width - 1u)));
  const uint32_t clampedY = static_cast<uint32_t>(std::clamp<int32_t>(y, 0, static_cast<int32_t>(height - 1u)));
  const size_t texelIndex = static_cast<size_t>(clampedY) * width + clampedX;
  if (texelIndex >= mip.texels.size())
    return {1.0f, 1.0f, 1.0f, 1.0f};
  return unpackRGBA8(mip.texels[texelIndex]);
}

std::array<float, 4> sampleBaseColorTexture(
    const BakedMaterialSurface &surface,
    const float uv[2],
    float mipLevel)
{
  if (surface.baseColorTexture.mips.empty())
    return {1.0f, 1.0f, 1.0f, 1.0f};

  const uint32_t resolvedMip = clampTextureMipLevel(surface, mipLevel);
  const VirtualTextureSystem::PreparedMipLevel &mip = surface.baseColorTexture.mips[resolvedMip];
  const float wrappedU = applyTextureAddressMode(uv[0], surface.baseColorSampling.addressModeU);
  const float wrappedV = 1.0f - applyTextureAddressMode(uv[1], surface.baseColorSampling.addressModeV);
  const float width = static_cast<float>(std::max(mip.width, 1u));
  const float height = static_cast<float>(std::max(mip.height, 1u));

  if (surface.baseColorSampling.filterMode == VirtualTextureSystem::TextureFilterMode::Nearest)
  {
    const uint32_t x = std::min(static_cast<uint32_t>(wrappedU * width), std::max(mip.width, 1u) - 1u);
    const uint32_t y = std::min(static_cast<uint32_t>(wrappedV * height), std::max(mip.height, 1u) - 1u);
    return loadTextureMipTexel(mip, static_cast<int32_t>(x), static_cast<int32_t>(y));
  }

  const float texelX = wrappedU * width - 0.5f;
  const float texelY = wrappedV * height - 0.5f;
  const int32_t baseX = static_cast<int32_t>(std::floor(texelX));
  const int32_t baseY = static_cast<int32_t>(std::floor(texelY));
  const float fracX = texelX - static_cast<float>(baseX);
  const float fracY = texelY - static_cast<float>(baseY);
  const std::array<float, 4> c00 = loadTextureMipTexel(mip, baseX, baseY);
  const std::array<float, 4> c10 = loadTextureMipTexel(mip, baseX + 1, baseY);
  const std::array<float, 4> c01 = loadTextureMipTexel(mip, baseX, baseY + 1);
  const std::array<float, 4> c11 = loadTextureMipTexel(mip, baseX + 1, baseY + 1);

  std::array<float, 4> result{};
  for (uint32_t channel = 0u; channel < 4u; ++channel)
  {
    const float x0 = c00[channel] + (c10[channel] - c00[channel]) * fracX;
    const float x1 = c01[channel] + (c11[channel] - c01[channel]) * fracX;
    result[channel] = x0 + (x1 - x0) * fracY;
  }
  return result;
}

void accumulateBaseColorSample(
    VertexSurfaceAccumulator &accumulator,
    const BakedMaterialSurface &surface,
    const FallbackMeshVertex &vertex,
    float mipLevel)
{
  const std::array<float, 4> textureColor = sampleBaseColorTexture(surface, vertex.uv, mipLevel);
  accumulator.baseColorOpacity[0] += surface.baseColorOpacity[0] * textureColor[0];
  accumulator.baseColorOpacity[1] += surface.baseColorOpacity[1] * textureColor[1];
  accumulator.baseColorOpacity[2] += surface.baseColorOpacity[2] * textureColor[2];
  accumulator.baseColorOpacity[3] += surface.baseColorOpacity[3] * textureColor[3];
  accumulator.weight += 1.0f;
}

std::vector<BakedMaterialSurface> buildBakedMaterialTable(
    const std::vector<std::string> &materialFiles,
    const std::string &basePath,
    const FallbackMeshBuilder::SurfaceBakeSettings &settings)
{
  std::vector<BakedMaterialSurface> surfaces(std::max<size_t>(materialFiles.size(), 1u));
  if (!settings.loadMaterialFiles)
    return surfaces;

  for (size_t materialIndex = 0u; materialIndex < materialFiles.size(); ++materialIndex)
  {
    if (materialFiles[materialIndex].empty())
      continue;

    std::filesystem::path materialPath(materialFiles[materialIndex]);
    if (materialPath.is_relative() && !basePath.empty())
      materialPath = std::filesystem::path(basePath) / materialPath;

    Material::Definition definition{};
    if (VirtualMaterialFile::loadDefinition(materialPath.lexically_normal().string(), definition))
    {
      surfaces[materialIndex] = makeBakedMaterialSurface(definition);
      const uint32_t baseColorTextureSlot = findBaseColorTextureSlot(definition);
      if (baseColorTextureSlot != Material::InvalidTextureId)
      {
        surfaces[materialIndex].baseColorSampling = definition.textures[baseColorTextureSlot].sampling;
        loadMaterialTexture(definition, materialPath, baseColorTextureSlot, surfaces[materialIndex].baseColorTexture);
      }
    }
  }

  return surfaces;
}

void applyBakedSurfaceToVertex(FallbackMeshVertex &vertex, const BakedMaterialSurface &surface)
{
  for (uint32_t channel = 0u; channel < 4u; ++channel)
  {
    vertex.surfaceBaseColorOpacity[channel] = surface.baseColorOpacity[channel];
    vertex.surfaceNormalRoughness[channel] = surface.normalRoughness[channel];
    vertex.surfaceEmissiveMetallic[channel] = surface.emissiveMetallic[channel];
    vertex.surfaceFlags[channel] = surface.flags[channel];
  }
  vertex.surfaceNormalRoughness[0] = vertex.normal[0];
  vertex.surfaceNormalRoughness[1] = vertex.normal[1];
  vertex.surfaceNormalRoughness[2] = vertex.normal[2];
}

float computeTriangleUvRadius(
    const FallbackMeshVertex &v0,
    const FallbackMeshVertex &v1,
    const FallbackMeshVertex &v2,
    float outCenter[2])
{
  outCenter[0] = (v0.uv[0] + v1.uv[0] + v2.uv[0]) / 3.0f;
  outCenter[1] = (v0.uv[1] + v1.uv[1] + v2.uv[1]) / 3.0f;

  float radius = 0.0f;
  const FallbackMeshVertex *vertices[3] = {&v0, &v1, &v2};
  for (const FallbackMeshVertex *vertex : vertices)
  {
    const float du = vertex->uv[0] - outCenter[0];
    const float dv = vertex->uv[1] - outCenter[1];
    radius = std::max(radius, std::sqrt(du * du + dv * dv));
  }
  return radius;
}

void bakeSurfaceDataWithSurfaces(
    FallbackMeshData &mesh,
    const FallbackMeshBuilder::SurfaceBakeSettings &settings,
    const std::vector<BakedMaterialSurface> &surfaces)
{
  std::vector<uint32_t> vertexMaterial(mesh.vertices.size(), 0u);

  if (settings.bakeVertexAttributes)
  {
    for (const FallbackMeshSection &section : mesh.sections)
    {
      const uint32_t indexEnd = std::min<uint32_t>(section.indexOffset + section.indexCount, static_cast<uint32_t>(mesh.indices.size()));
      for (uint32_t index = section.indexOffset; index < indexEnd; ++index)
      {
        const uint32_t vertexIndex = mesh.indices[index];
        if (vertexIndex < vertexMaterial.size())
          vertexMaterial[vertexIndex] = section.materialIndex;
      }
    }

    for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(mesh.vertices.size()); ++vertexIndex)
    {
      const uint32_t materialIndex = vertexMaterial[vertexIndex] < surfaces.size() ? vertexMaterial[vertexIndex] : 0u;
      applyBakedSurfaceToVertex(mesh.vertices[vertexIndex], surfaces[materialIndex]);
    }
  }

  if (!settings.bakeTriangleMipHints && !settings.bakeVertexAttributes)
  {
    mesh.triangleSurfaceHints.clear();
    return;
  }

  std::vector<VertexSurfaceAccumulator> vertexSurfaceAccumulators(mesh.vertices.size());
  mesh.triangleSurfaceHints.assign(mesh.indices.size() / 3u, FallbackMeshTriangleSurfaceHint{});
  for (const FallbackMeshSection &section : mesh.sections)
  {
    const uint32_t materialIndex = section.materialIndex < surfaces.size() ? section.materialIndex : 0u;
    const BakedMaterialSurface &surface = surfaces[materialIndex];
    const uint32_t triangleBegin = section.indexOffset / 3u;
    const uint32_t triangleEnd = std::min<uint32_t>((section.indexOffset + section.indexCount) / 3u, static_cast<uint32_t>(mesh.triangleSurfaceHints.size()));
    for (uint32_t triangleIndex = triangleBegin; triangleIndex < triangleEnd; ++triangleIndex)
    {
      const uint32_t i0 = mesh.indices[triangleIndex * 3u + 0u];
      const uint32_t i1 = mesh.indices[triangleIndex * 3u + 1u];
      const uint32_t i2 = mesh.indices[triangleIndex * 3u + 2u];
      if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
        continue;

      FallbackMeshTriangleSurfaceHint &hint = mesh.triangleSurfaceHints[triangleIndex];
      hint.materialIndex = section.materialIndex;
      hint.uvCircleRadius = computeTriangleUvRadius(mesh.vertices[i0], mesh.vertices[i1], mesh.vertices[i2], hint.uvCircleCenter);
      hint.textureMipLevel = computeOnePixelMipLevel(hint.uvCircleRadius, surface, settings);

      if (settings.bakeVertexAttributes)
      {
        accumulateBaseColorSample(vertexSurfaceAccumulators[i0], surface, mesh.vertices[i0], hint.textureMipLevel);
        accumulateBaseColorSample(vertexSurfaceAccumulators[i1], surface, mesh.vertices[i1], hint.textureMipLevel);
        accumulateBaseColorSample(vertexSurfaceAccumulators[i2], surface, mesh.vertices[i2], hint.textureMipLevel);
      }
    }
  }

  if (settings.bakeVertexAttributes)
  {
    for (uint32_t vertexIndex = 0u; vertexIndex < static_cast<uint32_t>(mesh.vertices.size()); ++vertexIndex)
    {
      const VertexSurfaceAccumulator &accumulator = vertexSurfaceAccumulators[vertexIndex];
      if (accumulator.weight <= 0.0f)
        continue;

      const float invWeight = 1.0f / accumulator.weight;
      for (uint32_t channel = 0u; channel < 4u; ++channel)
        mesh.vertices[vertexIndex].surfaceBaseColorOpacity[channel] = accumulator.baseColorOpacity[channel] * invWeight;
    }
  }

  if (!settings.bakeTriangleMipHints)
    mesh.triangleSurfaceHints.clear();
}

void bakeSurfaceData(
    FallbackMeshData &mesh,
    const FallbackMeshBuilder::SurfaceBakeSettings &settings,
    const std::string &basePath = {})
{
  const std::vector<BakedMaterialSurface> surfaces = buildBakedMaterialTable(mesh.materialFiles, basePath, settings);
  bakeSurfaceDataWithSurfaces(mesh, settings, surfaces);
}

size_t getTotalIndexCount(const std::vector<SectionGeometry> &sections)
{
  size_t totalIndexCount = 0u;
  for (const SectionGeometry &section : sections)
    totalIndexCount += section.indices.size();
  return totalIndexCount;
}

size_t getNonEmptySectionCount(const std::vector<SectionGeometry> &sections)
{
  size_t nonEmptySectionCount = 0u;
  for (const SectionGeometry &section : sections)
    nonEmptySectionCount += !section.indices.empty();
  return nonEmptySectionCount;
}

size_t getRatioTargetIndexCount(const std::vector<SectionGeometry> &sections, float ratio)
{
  const size_t totalIndexCount = getTotalIndexCount(sections);
  const size_t nonEmptySectionCount = getNonEmptySectionCount(sections);
  if (totalIndexCount == 0u || nonEmptySectionCount == 0u)
    return 0u;

  const double clampedRatio = std::clamp(static_cast<double>(ratio), 0.0, 1.0);
  size_t targetIndexCount = static_cast<size_t>(std::floor(static_cast<double>(totalIndexCount) * clampedRatio));
  targetIndexCount = (targetIndexCount / 3u) * 3u;
  targetIndexCount = std::max(nonEmptySectionCount * 3u, targetIndexCount);
  return std::min(totalIndexCount, targetIndexCount);
}

std::vector<size_t> distributeTargetIndexCounts(const std::vector<SectionGeometry> &sections, size_t totalTargetIndexCount)
{
  std::vector<size_t> targets(sections.size(), 0u);
  const size_t totalIndexCount = getTotalIndexCount(sections);
  const size_t nonEmptySectionCount = getNonEmptySectionCount(sections);
  if (totalIndexCount == 0u || nonEmptySectionCount == 0u)
    return targets;

  const size_t clampedTargetIndexCount = std::min(totalIndexCount, std::max(nonEmptySectionCount * 3u, (totalTargetIndexCount / 3u) * 3u));
  size_t remainingBudget = clampedTargetIndexCount;
  size_t totalExtraCapacity = 0u;

  for (size_t sectionIndex = 0u; sectionIndex < sections.size(); ++sectionIndex)
  {
    if (sections[sectionIndex].indices.empty())
      continue;
    targets[sectionIndex] = 3u;
    remainingBudget -= 3u;
    totalExtraCapacity += sections[sectionIndex].indices.size() - 3u;
  }

  struct RemainderEntry
  {
    size_t sectionIndex = 0u;
    double remainder = 0.0;
  };

  std::vector<RemainderEntry> remainders;
  remainders.reserve(sections.size());

  if (remainingBudget > 0u && totalExtraCapacity > 0u)
  {
    for (size_t sectionIndex = 0u; sectionIndex < sections.size(); ++sectionIndex)
    {
      const size_t currentIndexCount = sections[sectionIndex].indices.size();
      if (currentIndexCount <= 3u)
        continue;

      const size_t extraCapacity = currentIndexCount - 3u;
      const double exactShare = static_cast<double>(extraCapacity) / static_cast<double>(totalExtraCapacity) * static_cast<double>(remainingBudget);
      const size_t roundedShare = std::min(extraCapacity, static_cast<size_t>(exactShare / 3.0) * 3u);
      targets[sectionIndex] += roundedShare;
      remainders.push_back({sectionIndex, exactShare - static_cast<double>(roundedShare)});
    }

    size_t consumedBudget = 0u;
    for (size_t target : targets)
      consumedBudget += target;
    remainingBudget = clampedTargetIndexCount - consumedBudget;

    std::sort(
        remainders.begin(),
        remainders.end(),
        [](const RemainderEntry &left, const RemainderEntry &right)
        {
          if (left.remainder != right.remainder)
            return left.remainder > right.remainder;
          return left.sectionIndex < right.sectionIndex;
        });

    while (remainingBudget >= 3u)
    {
      bool assigned = false;
      for (RemainderEntry &entry : remainders)
      {
        const size_t sectionIndex = entry.sectionIndex;
        if (targets[sectionIndex] + 3u > sections[sectionIndex].indices.size())
          continue;

        targets[sectionIndex] += 3u;
        remainingBudget -= 3u;
        assigned = true;
        if (remainingBudget < 3u)
          break;
      }

      if (!assigned)
        break;
    }
  }

  for (size_t sectionIndex = 0u; sectionIndex < sections.size(); ++sectionIndex)
    targets[sectionIndex] = std::min(targets[sectionIndex], sections[sectionIndex].indices.size());

  return targets;
}

bool simplifySection(
    const SectionGeometry &input,
    size_t targetIndexCount,
    float targetError,
    const SimplifySectionOptions &options,
    SectionGeometry &output,
    float &outRelativeError)
{
  output = input;
  outRelativeError = 0.0f;

  if (input.vertices.empty() || input.indices.empty() || targetIndexCount >= input.indices.size())
    return true;

  std::vector<float> attributes;
  std::vector<float> attributeWeights;
  buildSimplifierAttributes(input.vertices, attributes, attributeWeights);

  const size_t attributeStride = sizeof(float) * (kBaseAttributeCount + getMaxBoneInfluenceCount(input.vertices) * 2u);
  uint32_t simplifyOptions = hasManySeams(input.vertices) ? meshopt_SimplifyPermissive : 0u;
  if (options.lockBorders)
    simplifyOptions |= meshopt_SimplifyLockBorder;
  if (options.regularize)
    simplifyOptions |= meshopt_SimplifyRegularize;

  const std::vector<unsigned char> vertexLocks = options.lockBoundsExtrema ? buildProxyVertexLocks(input.vertices, input.indices) : std::vector<unsigned char>{};
  const unsigned char *vertexLockData = vertexLocks.empty() ? nullptr : vertexLocks.data();

  auto simplifyWithError = [&](float errorLimit, uint32_t simplifyOptions, std::vector<uint32_t> &outIndices, float &outError) -> size_t
  {
    outIndices.assign(input.indices.size(), 0u);
    return meshopt_simplifyWithAttributes(
        outIndices.data(),
        input.indices.data(),
        input.indices.size(),
        &input.vertices[0].pos[0],
        input.vertices.size(),
        sizeof(Vertex),
        attributes.empty() ? nullptr : attributes.data(),
        attributes.empty() ? 0u : attributeStride,
        attributeWeights.empty() ? nullptr : attributeWeights.data(),
        attributeWeights.size(),
        vertexLockData,
        targetIndexCount,
        errorLimit,
        simplifyOptions,
        &outError);
  };

  std::vector<uint32_t> simplifiedIndices;
  size_t simplifiedIndexCount = simplifyWithError(targetError, simplifyOptions, simplifiedIndices, outRelativeError);

  if (simplifiedIndexCount > targetIndexCount)
  {
    std::vector<uint32_t> relaxedIndices;
    float relaxedRelativeError = 0.0f;
    const size_t relaxedIndexCount = simplifyWithError(std::numeric_limits<float>::max(), simplifyOptions | meshopt_SimplifyPermissive, relaxedIndices, relaxedRelativeError);
    if (relaxedIndexCount != 0u && relaxedIndexCount < simplifiedIndexCount)
    {
      simplifiedIndices = std::move(relaxedIndices);
      simplifiedIndexCount = relaxedIndexCount;
      outRelativeError = relaxedRelativeError;
    }
  }

  if (simplifiedIndexCount == 0u)
    return false;

  simplifiedIndices.resize(simplifiedIndexCount);
  meshopt_optimizeVertexCache(simplifiedIndices.data(), simplifiedIndices.data(), simplifiedIndices.size(), input.vertices.size());

  std::vector<uint32_t> oldToNew(input.vertices.size(), UINT32_MAX);
  output.vertices.clear();
  output.indices = std::move(simplifiedIndices);
  for (uint32_t &index : output.indices)
  {
    if (oldToNew[index] == UINT32_MAX)
    {
      oldToNew[index] = static_cast<uint32_t>(output.vertices.size());
      output.vertices.push_back(input.vertices[index]);
    }
    index = oldToNew[index];
  }

  return true;
}

FallbackMeshVertex convertVertex(const Vertex &vertex)
{
  FallbackMeshVertex result;
  result.position[0] = vertex.pos[0];
  result.position[1] = vertex.pos[1];
  result.position[2] = vertex.pos[2];
  result.normal[0] = vertex.norm[0];
  result.normal[1] = vertex.norm[1];
  result.normal[2] = vertex.norm[2];
  result.uv[0] = vertex.uv[0];
  result.uv[1] = vertex.uv[1];

  float totalWeight = 0.0f;
  const size_t influenceCount = std::min(vertex.boneWeights.size(), size_t(4u));
  for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
  {
    result.boneIndices[influenceIndex] = vertex.boneWeights[influenceIndex].boneIndex;
    result.boneWeights[influenceIndex] = vertex.boneWeights[influenceIndex].weight;
    totalWeight += result.boneWeights[influenceIndex];
  }

  if (influenceCount == 0u)
  {
    result.boneWeights[0] = 1.0f;
    return result;
  }

  if (totalWeight > 0.0f)
    for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
      result.boneWeights[influenceIndex] /= totalWeight;

  return result;
}

// Compute the minimum enclosing capsule for a set of points using a simple
// iterative approach: choose the pair of extreme points along the principal
// axis (eigenvector of covariance or just max-spread axis), fit a capsule
// with that axis, then check all points and expand radius as needed.
//
// For bone capsules an exact solution is not needed — the bounds are used
// conservatively, so a slight overestimate is acceptable.
static void computeBoneCapsule(
    const std::vector<math::Vec3f> &points,
    math::Vec3f &outStart,
    math::Vec3f &outEnd,
    float &outRadius)
{
  outStart  = math::Vec3f(0.0f, 0.0f, 0.0f);
  outEnd    = math::Vec3f(0.0f, 0.0f, 0.0f);
  outRadius = 0.0f;

  if (points.empty())
    return;

  // Find the axis of greatest spread (AABB diagonal direction).
  math::Vec3f minP = points[0];
  math::Vec3f maxP = points[0];
  for (const math::Vec3f &p : points)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      minP[axis] = std::min(minP[axis], p[axis]);
      maxP[axis] = std::max(maxP[axis], p[axis]);
    }
  }

  const math::Vec3f diag = maxP - minP;
  // Select the axis with the most spread as the capsule direction.
  uint32_t maxAxis = 0u;
  if (diag[1] > diag[maxAxis]) maxAxis = 1u;
  if (diag[2] > diag[maxAxis]) maxAxis = 2u;

  // Capsule endpoints: extreme projections along maxAxis, centred on the AABB
  // midpoint in the perpendicular axes.
  float projMin = std::numeric_limits<float>::max();
  float projMax = -std::numeric_limits<float>::max();
  for (const math::Vec3f &p : points)
  {
    projMin = std::min(projMin, p[maxAxis]);
    projMax = std::max(projMax, p[maxAxis]);
  }

  const math::Vec3f centroid = (minP + maxP) * 0.5f;
  outStart = centroid;
  outStart[maxAxis] = projMin;
  outEnd = centroid;
  outEnd[maxAxis] = projMax;

  // Radius = max perpendicular distance from any point to the axis segment.
  float maxRadiusSq = 0.0f;
  for (const math::Vec3f &p : points)
  {
    const float proj = p[maxAxis];
    const float clampedProj = std::clamp(proj, projMin, projMax);
    math::Vec3f closest = outStart;
    closest[maxAxis] = clampedProj;
    const math::Vec3f diff = p - closest;
    const float distSq = diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2];
    maxRadiusSq = std::max(maxRadiusSq, distSq);
  }
  outRadius = std::sqrt(maxRadiusSq);
}

void computeBounds(
    const std::vector<FallbackMeshVertex> &vertices,
    float boundsMin[3],
    float boundsMax[3])
{
  if (vertices.empty())
  {
    boundsMin[0] = boundsMin[1] = boundsMin[2] = 0.0f;
    boundsMax[0] = boundsMax[1] = boundsMax[2] = 0.0f;
    return;
  }

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    boundsMin[axis] = std::numeric_limits<float>::max();
    boundsMax[axis] = -std::numeric_limits<float>::max();
  }

  for (const FallbackMeshVertex &vertex : vertices)
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      boundsMin[axis] = std::min(boundsMin[axis], vertex.position[axis]);
      boundsMax[axis] = std::max(boundsMax[axis], vertex.position[axis]);
    }
}

FallbackMeshData buildLOD(
    const std::vector<SectionGeometry> &sections,
    const std::vector<std::string> &materialFiles,
    const rendering::animation::Skeleton &skeleton,
    uint32_t lodLevel,
    float targetError,
    float relativeError)
{
  FallbackMeshData data;
  data.lodLevel = lodLevel;
  data.targetError = targetError;
  data.relativeError = relativeError;
  data.materialFiles = materialFiles;
  data.skeleton = skeleton;

  for (const SectionGeometry &section : sections)
  {
    FallbackMeshSection sectionDescriptor;
    sectionDescriptor.indexOffset = static_cast<uint32_t>(data.indices.size());
    sectionDescriptor.indexCount = static_cast<uint32_t>(section.indices.size());
    sectionDescriptor.materialIndex = section.materialIndex;

    const uint32_t vertexOffset = static_cast<uint32_t>(data.vertices.size());
    data.sections.push_back(sectionDescriptor);

    for (const Vertex &vertex : section.vertices)
      data.vertices.push_back(convertVertex(vertex));
    for (uint32_t index : section.indices)
      data.indices.push_back(vertexOffset + index);
  }

  computeBounds(data.vertices, data.boundsMin, data.boundsMax);

  // Compute per-bone capsules from the assembled vertex data.
  const uint32_t boneCount = data.skeleton.getBoneCount();
  if (boneCount > 0u)
  {
    std::vector<std::vector<math::Vec3f>> bonePoints(boneCount);
    for (const FallbackMeshVertex &vertex : data.vertices)
    {
      for (uint32_t influenceIndex = 0u; influenceIndex < 4u; ++influenceIndex)
      {
        if (vertex.boneWeights[influenceIndex] <= 0.0f)
          continue;
        const uint32_t boneIndex = vertex.boneIndices[influenceIndex];
        if (boneIndex >= boneCount)
          continue;
        bonePoints[boneIndex].push_back(
            math::Vec3f(vertex.position[0], vertex.position[1], vertex.position[2]));
      }
    }

    for (uint32_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
    {
      rendering::animation::Skeleton::Bone &bone = data.skeleton.getBone(boneIndex);
      computeBoneCapsule(
          bonePoints[boneIndex],
          bone.capsuleStart,
          bone.capsuleEnd,
          bone.capsuleRadius);
    }
  }

  return data;
}

FallbackMeshData buildProxyFromSections(
    const std::vector<SectionGeometry> &sections,
    const std::vector<std::string> &materialFiles,
    const rendering::animation::Skeleton &skeleton,
    const FallbackMeshBuilder::Settings &settings)
{
  const size_t totalIndexCount = getTotalIndexCount(sections);
  const size_t nonEmptySectionCount = getNonEmptySectionCount(sections);
  if (totalIndexCount == 0u || nonEmptySectionCount == 0u)
    return buildLOD(sections, materialFiles, skeleton, 0u, settings.proxyTargetError, 0.0f);

  const size_t ratioTargetIndexCount = getRatioTargetIndexCount(sections, settings.proxyTargetTriangleRatio);
  const size_t minProxyIndexCount = static_cast<size_t>(settings.proxyMinTriangleCount) * 3u * nonEmptySectionCount;
  const size_t targetIndexCount = std::min(totalIndexCount, std::max(ratioTargetIndexCount, minProxyIndexCount));
  const std::vector<size_t> sectionTargets = distributeTargetIndexCounts(sections, targetIndexCount);

  const SimplifySectionOptions simplifyOptions{
      .lockBorders = false,
      .regularize = true,
      .lockBoundsExtrema = true,
  };

  std::vector<SectionGeometry> proxySections;
  proxySections.reserve(sections.size());
  float maxRelativeError = 0.0f;

  for (size_t sectionIndex = 0u; sectionIndex < sections.size(); ++sectionIndex)
  {
    SectionGeometry proxySection;
    float sectionRelativeError = 0.0f;
    if (!simplifySection(sections[sectionIndex], sectionTargets[sectionIndex], settings.proxyTargetError, simplifyOptions, proxySection, sectionRelativeError))
      proxySection = sections[sectionIndex];
    maxRelativeError = std::max(maxRelativeError, sectionRelativeError);
    proxySections.push_back(std::move(proxySection));
  }

  return buildLOD(proxySections, materialFiles, skeleton, 0u, settings.proxyTargetError, maxRelativeError);
}

} // namespace

FallbackMeshData FallbackMeshBuilder::buildOriginal(const VirtualGeometrySourceAsset &asset)
{
  const std::vector<SectionGeometry> sections = buildSourceSections(asset);
  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  return buildLOD(sections, materialFiles, asset.skeleton, 0u, 0.0f, 0.0f);
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildOriginalPerShape(const VirtualGeometrySourceAsset &asset)
{
  const std::vector<SectionGeometry> sections = buildSourceSections(asset);
  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  if (sections.empty())
  {
    return std::vector<FallbackMeshData>{buildLOD(sections, materialFiles, asset.skeleton, 0u, 0.0f, 0.0f)};
  }

  std::vector<FallbackMeshData> meshes;
  meshes.reserve(sections.size());
  for (const SectionGeometry &section : sections)
  {
    meshes.push_back(buildLOD(std::vector<SectionGeometry>{section}, materialFiles, asset.skeleton, 0u, 0.0f, 0.0f));
  }
  return meshes;
}

FallbackMeshData FallbackMeshBuilder::buildSDFOriginal(const VirtualGeometrySourceAsset &asset)
{
  return buildSDFOriginal(asset, SurfaceBakeSettings{});
}

FallbackMeshData FallbackMeshBuilder::buildSDFOriginal(
    const VirtualGeometrySourceAsset &asset,
    const SurfaceBakeSettings &surfaceSettings)
{
  FallbackMeshData mesh = buildOriginal(asset);
  bakeSurfaceData(mesh, surfaceSettings);
  return mesh;
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildSDFOriginalPerShape(const VirtualGeometrySourceAsset &asset)
{
  return buildSDFOriginalPerShape(asset, SurfaceBakeSettings{});
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildSDFOriginalPerShape(
    const VirtualGeometrySourceAsset &asset,
    const SurfaceBakeSettings &surfaceSettings)
{
  std::vector<FallbackMeshData> meshes = buildOriginalPerShape(asset);
  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  const std::vector<BakedMaterialSurface> surfaces = buildBakedMaterialTable(materialFiles, {}, surfaceSettings);
  parallelFor(
      meshes.size(),
      [&](size_t meshIndex)
      {
        bakeSurfaceDataWithSurfaces(meshes[meshIndex], surfaceSettings, surfaces);
      });
  return meshes;
}

FallbackMeshData FallbackMeshBuilder::buildProxy(const VirtualGeometrySourceAsset &asset)
{
  return buildProxy(asset, Settings{});
}

FallbackMeshData FallbackMeshBuilder::buildProxy(const VirtualGeometrySourceAsset &asset, const Settings &settings)
{
  const std::vector<SectionGeometry> sections = buildSourceSections(asset);
  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  return buildProxyFromSections(sections, materialFiles, asset.skeleton, settings);
}

FallbackMeshData FallbackMeshBuilder::buildSDFProxy(const VirtualGeometrySourceAsset &asset)
{
  return buildSDFProxy(asset, Settings{}, SurfaceBakeSettings{});
}

FallbackMeshData FallbackMeshBuilder::buildSDFProxy(
    const VirtualGeometrySourceAsset &asset,
    const Settings &settings)
{
  return buildSDFProxy(asset, settings, SurfaceBakeSettings{});
}

FallbackMeshData FallbackMeshBuilder::buildSDFProxy(
    const VirtualGeometrySourceAsset &asset,
    const Settings &settings,
    const SurfaceBakeSettings &surfaceSettings)
{
  FallbackMeshData mesh = buildProxy(asset, settings);
  bakeSurfaceData(mesh, surfaceSettings);
  return mesh;
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildProxyPerShape(const VirtualGeometrySourceAsset &asset)
{
  return buildProxyPerShape(asset, Settings{});
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildProxyPerShape(const VirtualGeometrySourceAsset &asset, const Settings &settings)
{
  const std::vector<SectionGeometry> sections = buildSourceSections(asset);
  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  if (sections.empty())
  {
    return std::vector<FallbackMeshData>{buildProxyFromSections(sections, materialFiles, asset.skeleton, settings)};
  }

  std::vector<FallbackMeshData> meshes;
  meshes.resize(sections.size());
  parallelFor(
      sections.size(),
      [&](size_t sectionIndex)
      {
        meshes[sectionIndex] = buildProxyFromSections(std::vector<SectionGeometry>{sections[sectionIndex]}, materialFiles, asset.skeleton, settings);
      });
  return meshes;
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildSDFProxyPerShape(const VirtualGeometrySourceAsset &asset)
{
  return buildSDFProxyPerShape(asset, Settings{}, SurfaceBakeSettings{});
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildSDFProxyPerShape(
    const VirtualGeometrySourceAsset &asset,
    const Settings &settings)
{
  return buildSDFProxyPerShape(asset, settings, SurfaceBakeSettings{});
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildSDFProxyPerShape(
    const VirtualGeometrySourceAsset &asset,
    const Settings &settings,
    const SurfaceBakeSettings &surfaceSettings)
{
  std::vector<FallbackMeshData> meshes = buildProxyPerShape(asset, settings);
  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  const std::vector<BakedMaterialSurface> surfaces = buildBakedMaterialTable(materialFiles, {}, surfaceSettings);
  parallelFor(
      meshes.size(),
      [&](size_t meshIndex)
      {
        bakeSurfaceDataWithSurfaces(meshes[meshIndex], surfaceSettings, surfaces);
      });
  return meshes;
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildLODs(
    const VirtualGeometrySourceAsset &asset)
{
  return buildLODs(asset, Settings{});
}

std::vector<FallbackMeshData> FallbackMeshBuilder::buildLODs(
    const VirtualGeometrySourceAsset &asset,
    const Settings &settings)
{
  std::vector<FallbackMeshData> lods;
  std::vector<SectionGeometry> currentSections = buildSourceSections(asset);
  if (currentSections.empty())
    return lods;

  const std::vector<std::string> materialFiles = buildMaterialFileTable(asset);
  size_t currentTotalIndexCount = getTotalIndexCount(currentSections);
  float targetError = settings.lod0TargetError;

  for (uint32_t lodLevel = 0u; lodLevel < settings.maxLODCount; ++lodLevel)
  {
    const size_t nonEmptySectionCount = getNonEmptySectionCount(currentSections);
    if (nonEmptySectionCount == 0u || currentTotalIndexCount == 0u)
      break;

    const size_t targetIndexCount = lodLevel == 0u
                                        ? getRatioTargetIndexCount(currentSections, settings.lod0TargetTriangleRatio)
                                        : std::max(nonEmptySectionCount * 3u, (currentTotalIndexCount / 6u) * 3u);
    const std::vector<size_t> sectionTargets = distributeTargetIndexCounts(currentSections, targetIndexCount);

    std::vector<SectionGeometry> nextSections;
    nextSections.reserve(currentSections.size());
    float maxRelativeError = 0.0f;

    for (size_t sectionIndex = 0u; sectionIndex < currentSections.size(); ++sectionIndex)
    {
      SectionGeometry simplifiedSection;
      float sectionRelativeError = 0.0f;
      if (!simplifySection(currentSections[sectionIndex], sectionTargets[sectionIndex], targetError, SimplifySectionOptions{}, simplifiedSection, sectionRelativeError))
        simplifiedSection = currentSections[sectionIndex];
      maxRelativeError = std::max(maxRelativeError, sectionRelativeError);
      nextSections.push_back(std::move(simplifiedSection));
    }

    lods.push_back(buildLOD(nextSections, materialFiles, asset.skeleton, lodLevel, targetError, maxRelativeError));

    currentSections = std::move(nextSections);
    currentTotalIndexCount = getTotalIndexCount(currentSections);
    if (currentTotalIndexCount <= nonEmptySectionCount * 3u)
      break;

    targetError *= settings.lodErrorMultiplier;
  }

  return lods;
}

} // namespace virtualgeometry
