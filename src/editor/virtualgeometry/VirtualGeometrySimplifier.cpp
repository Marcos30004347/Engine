#include "VirtualGeometrySimplifier.hpp"
#include "meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

using namespace virtualgeometry;

namespace
{

constexpr size_t kBaseAttributeCount = 5u;
constexpr size_t kMaxSimplifierInfluences = (32u - kBaseAttributeCount) / 2u;
constexpr float kNormalAttributeWeight = 0.5f;
constexpr float kUVAttributeWeight = 1.0f;
constexpr float kBoneIndexAttributeWeight = 4.0f;
constexpr float kBoneWeightAttributeWeight = 2.0f;

static size_t getMaxBoneInfluenceCount(const std::vector<Vertex> &vertices)
{
  size_t maxCount = 0u;
  for (const Vertex &vertex : vertices)
    maxCount = std::max(maxCount, vertex.boneWeights.size());
  return std::min(maxCount, kMaxSimplifierInfluences);
}

static bool containsBoneWeights(const std::vector<Vertex> &vertices)
{
  return getMaxBoneInfluenceCount(vertices) > 0u;
}

static uint32_t getMaxBoneIndex(const std::vector<Vertex> &vertices)
{
  uint32_t maxBoneIndex = 0u;
  for (const Vertex &vertex : vertices)
    for (const BoneWeight &boneWeight : vertex.boneWeights)
      maxBoneIndex = std::max(maxBoneIndex, boneWeight.boneIndex);
  return maxBoneIndex;
}

static void buildSimplifierAttributes(
    const std::vector<Vertex> &vertices,
    size_t influenceCount,
    float boneIndexScale,
    std::vector<float> &attributes,
    std::vector<float> &attributeWeights)
{
  const size_t attributeCount = kBaseAttributeCount + influenceCount * 2u;
  attributes.resize(vertices.size() * attributeCount, 0.0f);
  attributeWeights.assign(attributeCount, 0.0f);

  attributeWeights[0] = kNormalAttributeWeight;
  attributeWeights[1] = kNormalAttributeWeight;
  attributeWeights[2] = kNormalAttributeWeight;
  attributeWeights[3] = kUVAttributeWeight;
  attributeWeights[4] = kUVAttributeWeight;

  for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
  {
    attributeWeights[kBaseAttributeCount + influenceIndex * 2u + 0u] = kBoneIndexAttributeWeight;
    attributeWeights[kBaseAttributeCount + influenceIndex * 2u + 1u] = kBoneWeightAttributeWeight;
  }

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
      const size_t influenceBase = attributeBase + kBaseAttributeCount + influenceIndex * 2u;
      if (influenceIndex < vertex.boneWeights.size())
      {
        attributes[influenceBase + 0u] = static_cast<float>(vertex.boneWeights[influenceIndex].boneIndex) * boneIndexScale;
        attributes[influenceBase + 1u] = vertex.boneWeights[influenceIndex].weight;
      }
    }
  }
}

static void rebuildBoneWeightsFromAttributes(
    std::vector<Vertex> &vertices,
    const std::vector<float> &attributes,
    size_t influenceCount,
    float boneIndexScale,
    uint32_t maxBoneIndex)
{
  const size_t attributeCount = kBaseAttributeCount + influenceCount * 2u;
  const float inverseBoneIndexScale = boneIndexScale > 0.0f ? (1.0f / boneIndexScale) : 0.0f;

  for (size_t vertexIndex = 0u; vertexIndex < vertices.size(); ++vertexIndex)
  {
    Vertex &vertex = vertices[vertexIndex];
    const std::vector<BoneWeight> originalWeights = vertex.boneWeights;
    const size_t attributeBase = vertexIndex * attributeCount;

    vertex.norm[0] = attributes[attributeBase + 0u];
    vertex.norm[1] = attributes[attributeBase + 1u];
    vertex.norm[2] = attributes[attributeBase + 2u];
    vertex.uv[0] = attributes[attributeBase + 3u];
    vertex.uv[1] = attributes[attributeBase + 4u];

    if (influenceCount == 0u)
      continue;

    std::unordered_map<uint32_t, float> mergedWeights;
    for (size_t influenceIndex = 0u; influenceIndex < influenceCount; ++influenceIndex)
    {
      const size_t influenceBase = attributeBase + kBaseAttributeCount + influenceIndex * 2u;
      const float weight = std::max(0.0f, attributes[influenceBase + 1u]);
      if (weight <= 0.0f)
        continue;

      const float encodedBoneIndex = attributes[influenceBase + 0u];
      const uint32_t boneIndex = boneIndexScale > 0.0f
                                     ? std::min(maxBoneIndex, static_cast<uint32_t>(std::lround(encodedBoneIndex * inverseBoneIndexScale)))
                                     : 0u;
      mergedWeights[boneIndex] += weight;
    }

    std::vector<BoneWeight> rebuiltWeights;
    rebuiltWeights.reserve(mergedWeights.size());
    for (const auto &entry : mergedWeights)
      rebuiltWeights.push_back(BoneWeight{entry.second, entry.first});

    if (rebuiltWeights.empty())
      rebuiltWeights = originalWeights;

    if (rebuiltWeights.empty())
    {
      vertex.boneWeights.clear();
      continue;
    }

    std::sort(
        rebuiltWeights.begin(),
        rebuiltWeights.end(),
        [](const BoneWeight &lhs, const BoneWeight &rhs)
        {
          if (lhs.weight != rhs.weight)
            return lhs.weight > rhs.weight;
          return lhs.boneIndex < rhs.boneIndex;
        });

    if (rebuiltWeights.size() > influenceCount)
      rebuiltWeights.resize(influenceCount);

    float totalWeight = 0.0f;
    for (const BoneWeight &boneWeight : rebuiltWeights)
      totalWeight += boneWeight.weight;

    if (totalWeight > 0.0f)
      for (BoneWeight &boneWeight : rebuiltWeights)
        boneWeight.weight /= totalWeight;

    rebuiltWeights.resize(influenceCount, BoneWeight{0.0f, rebuiltWeights.front().boneIndex});
    vertex.boneWeights = std::move(rebuiltWeights);
  }
}

} // namespace

std::vector<uint32_t> VirtualGeometrySimplifier::simplify(
    const std::vector<Vertex>  &vertices,
    std::vector<Vertex>        &outVertices,
    const std::vector<uint32_t> &indices,
    const std::vector<uint8_t> *locks,
    uint64_t                    targetCount,
    float                      *outError)
{
  // ── 1. Build a compact local vertex buffer from the indices ───────────────
  // Sort + unique gives us a sorted list of referenced global indices, which
  // lets us use binary search for the global→local remap (no hash map needed).
  std::vector<uint32_t> uniqueGlobal = indices;
  std::sort(uniqueGlobal.begin(), uniqueGlobal.end());
  uniqueGlobal.erase(std::unique(uniqueGlobal.begin(), uniqueGlobal.end()), uniqueGlobal.end());

  std::vector<Vertex>  localVerts(uniqueGlobal.size());
  std::vector<uint8_t> localLocks;

  for (size_t i = 0; i < uniqueGlobal.size(); ++i)
    localVerts[i] = vertices[uniqueGlobal[i]];

  if (locks)
  {
    localLocks.resize(uniqueGlobal.size());
    for (size_t i = 0; i < uniqueGlobal.size(); ++i)
      localLocks[i] = (*locks)[uniqueGlobal[i]];
  }

  std::vector<uint32_t> localIndices(indices.size());
  for (size_t i = 0; i < indices.size(); ++i)
  {
    auto it       = std::lower_bound(uniqueGlobal.begin(), uniqueGlobal.end(), indices[i]);
    localIndices[i] = static_cast<uint32_t>(it - uniqueGlobal.begin());
  }

  // Early out — target already satisfied.
  if (targetCount >= localIndices.size())
  {
    outVertices = std::move(localVerts);
    return localIndices;
  }

  // ── 2. Simplify with vertex update ───────────────────────────────────────
  // meshopt_SimplifyErrorAbsolute: error is returned in world-space units,
  // which maps directly to a projection error at runtime:
  //   screenError = worldError / distance * cotHalfFov * screenHeight * 0.5
  //
  // UV weight is raised to 1.0 as recommended when using WithUpdate so that
  // texture coordinates are updated to avoid distortion. Skinning attributes
  // are packed into the same stream so simplification respects bone affinity.
  const uint32_t options = meshopt_SimplifyErrorAbsolute;
  const size_t influenceCount = getMaxBoneInfluenceCount(localVerts);
  const uint32_t maxBoneIndex = getMaxBoneIndex(localVerts);
  const float boneIndexScale = maxBoneIndex > 0u ? (1.0f / static_cast<float>(maxBoneIndex)) : 0.0f;
  std::vector<float> vertexAttributes;
  std::vector<float> attributeWeights;
  buildSimplifierAttributes(localVerts, influenceCount, boneIndexScale, vertexAttributes, attributeWeights);
  float resultError = 0.f;

  size_t resultSize = meshopt_simplifyWithUpdate(
      localIndices.data(), localIndices.size(),
      &localVerts[0].pos[0],  localVerts.size(), sizeof(Vertex),
      vertexAttributes.data(), sizeof(float) * (kBaseAttributeCount + influenceCount * 2u),
      attributeWeights.data(), attributeWeights.size(),
      locks ? localLocks.data() : nullptr,
      targetCount, std::numeric_limits<float>::max(),
      options, &resultError);

  localIndices.resize(resultSize);
  if (outError) *outError = resultError;

  rebuildBoneWeightsFromAttributes(localVerts, vertexAttributes, influenceCount, boneIndexScale, maxBoneIndex);

  // ── 3. Renormalize normals (required after WithUpdate) ───────────────────
  for (auto &v : localVerts)
  {
    float len = std::sqrt(v.norm[0] * v.norm[0] + v.norm[1] * v.norm[1] + v.norm[2] * v.norm[2]);
    if (len > 1e-6f) { v.norm[0] /= len; v.norm[1] /= len; v.norm[2] /= len; }
  }

  // ── 4. Compact: drop unreferenced vertices, vertex-cache-optimise first ──
  // We cannot use meshopt_optimizeVertexFetch because Vertex contains a
  // std::vector member; we do the remap manually instead.
  meshopt_optimizeVertexCache(localIndices.data(), localIndices.data(), localIndices.size(), localVerts.size());

  std::vector<uint32_t> oldToNew(localVerts.size(), UINT32_MAX);
  outVertices.clear();
  for (uint32_t idx : localIndices)
    if (oldToNew[idx] == UINT32_MAX)
    {
      oldToNew[idx] = static_cast<uint32_t>(outVertices.size());
      outVertices.push_back(std::move(localVerts[idx]));
    }
  for (uint32_t &idx : localIndices)
    idx = oldToNew[idx];

  return localIndices;
}

std::vector<uint32_t> VirtualGeometrySimplifier::simplifySloppy(
    const std::vector<Vertex>  &vertices,
    std::vector<Vertex>        &outVertices,
    const std::vector<uint32_t> &indices,
    const std::vector<uint8_t> *locks,
    uint64_t                    targetCount,
    float                      *outError)
{
  // ── 1. Extract local vertex buffer ───────────────────────────────────────
  std::vector<uint32_t> uniqueGlobal = indices;
  std::sort(uniqueGlobal.begin(), uniqueGlobal.end());
  uniqueGlobal.erase(std::unique(uniqueGlobal.begin(), uniqueGlobal.end()), uniqueGlobal.end());

  std::vector<Vertex> localVerts(uniqueGlobal.size());
  std::vector<uint8_t> localLocks(uniqueGlobal.size(), 0);
  bool hasLockedVertex = false;
  for (size_t i = 0; i < uniqueGlobal.size(); ++i)
  {
    localVerts[i] = vertices[uniqueGlobal[i]];
    if (locks)
    {
      localLocks[i] = (*locks)[uniqueGlobal[i]];
      hasLockedVertex = hasLockedVertex || (localLocks[i] != 0);
    }
  }

  std::vector<uint32_t> localIndices(indices.size());
  for (size_t i = 0; i < indices.size(); ++i)
  {
    auto it         = std::lower_bound(uniqueGlobal.begin(), uniqueGlobal.end(), indices[i]);
    localIndices[i] = static_cast<uint32_t>(it - uniqueGlobal.begin());
  }

  if (targetCount >= localIndices.size())
  {
    outVertices = std::move(localVerts);
    return localIndices;
  }

  // Locks must always be respected; sloppy has no lock support.
  if (hasLockedVertex || containsBoneWeights(localVerts))
    return simplify(vertices, outVertices, indices, locks, targetCount, outError);

  // ── 2. Sloppy simplification (no locked vertices in this set) ─────────────
  float resultErrorRel = 0.f;
  std::vector<uint32_t> simplified(localIndices.size());
  size_t resultSize = meshopt_simplifySloppy(
      simplified.data(),
      localIndices.data(), localIndices.size(),
      &localVerts[0].pos[0], localVerts.size(), sizeof(Vertex),
      targetCount, 1.0f, &resultErrorRel);

  simplified.resize(resultSize);

  // Convert relative error → absolute (world-space units) for consistency
  // with simplify(), so callers can accumulate errors in the same units.
  if (outError)
    *outError = resultErrorRel * meshopt_simplifyScale(
        &localVerts[0].pos[0], localVerts.size(), sizeof(Vertex));

  // ── 3. Compact ───────────────────────────────────────────────────────────
  meshopt_optimizeVertexCache(simplified.data(), simplified.data(), simplified.size(), localVerts.size());

  std::vector<uint32_t> oldToNew(localVerts.size(), UINT32_MAX);
  outVertices.clear();
  for (uint32_t idx : simplified)
    if (oldToNew[idx] == UINT32_MAX)
    {
      oldToNew[idx] = static_cast<uint32_t>(outVertices.size());
      outVertices.push_back(std::move(localVerts[idx]));
    }
  for (uint32_t &idx : simplified)
    idx = oldToNew[idx];

  return simplified;
}
