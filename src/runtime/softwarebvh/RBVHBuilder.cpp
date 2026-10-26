#include "softwarebvh/RBVHBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace softwarebvh
{

namespace
{

constexpr uint32_t kMaxClippedPrimitiveVertices = 16u;

struct RBVHBuildPrimitive
{
  uint32_t primitiveIndex = CWBVHBuilder::INVALID_PRIMITIVE_INDEX;
  uint32_t componentId = 0u;
  virtualgeometry::AABB bounds{};
  float centroid[3]{0.0f, 0.0f, 0.0f};
  float normal[3]{0.0f, 0.0f, 1.0f};
  float vertices[kMaxClippedPrimitiveVertices][3]{};
  float area = 0.0f;
  uint32_t vertexCount = 0u;
  bool hasNormal = false;
};

static float dot3(const float a[3], const float b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

static float length3(const float v[3])
{
  return std::sqrt(std::max(0.0f, dot3(v, v)));
}

static bool normalize3(float v[3])
{
  const float len = length3(v);
  if (!(len > 1.0e-12f) || !std::isfinite(len))
  {
    return false;
  }
  const float invLen = 1.0f / len;
  v[0] *= invLen;
  v[1] *= invLen;
  v[2] *= invLen;
  return true;
}

static void copy3(const float src[3], float dst[3])
{
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
}

static void sub3(const float a[3], const float b[3], float out[3])
{
  out[0] = a[0] - b[0];
  out[1] = a[1] - b[1];
  out[2] = a[2] - b[2];
}

static float clampUnit(const float value)
{
  return std::clamp(value, -1.0f, 1.0f);
}

static uint32_t nextPowerOfTwo(const uint32_t value)
{
  if (value <= 1u)
  {
    return 1u;
  }
  uint32_t result = 1u;
  while (result < value)
  {
    result <<= 1u;
  }
  return result;
}

static uint32_t alignUp(const uint32_t value, const uint32_t alignment)
{
  if (alignment <= 1u)
  {
    return value;
  }
  return ((value + alignment - 1u) / alignment) * alignment;
}

static uint32_t cardMipAlignment(const RBVHCardPrimitive &card)
{
  return nextPowerOfTwo(std::max(std::max(card.atlasWidth, card.atlasHeight), 1u));
}

static uint32_t floorLog2U32(const uint32_t value)
{
  uint32_t result = 0u;
  uint32_t shifted = value;
  while (shifted > 1u)
  {
    shifted >>= 1u;
    ++result;
  }
  return result;
}

static uint32_t ceilLog2U32(const uint32_t value)
{
  if (value <= 1u)
  {
    return 0u;
  }
  return floorLog2U32(value - 1u) + 1u;
}

static uint32_t rbvhDepthMipLevelOffset(const uint32_t atlasSize, const uint32_t mipLevel)
{
  uint32_t offset = 0u;
  uint32_t levelSize = std::max(atlasSize >> 1u, 1u);
  for (uint32_t level = 1u; level < mipLevel; ++level)
  {
    offset += levelSize * levelSize;
    levelSize = std::max(levelSize >> 1u, 1u);
  }
  return offset;
}

static uint32_t rbvhCardCoarseMipLevel(const uint32_t atlasSize, const uint32_t cardAtlasSize)
{
  const uint32_t atlasMipLevelCount = atlasSize > 1u ? floorLog2U32(atlasSize) : 0u;
  const uint32_t cardMipLevelCount = ceilLog2U32(cardAtlasSize);
  return std::min(atlasMipLevelCount, cardMipLevelCount);
}

static uint32_t packRbvhCardMipMetadata(const uint32_t atlasSize, const uint32_t cardAtlasSize)
{
  const uint32_t rootMipLevel = std::min(rbvhCardCoarseMipLevel(atlasSize, cardAtlasSize), 31u);
  const uint32_t rootMipOffset = rootMipLevel == 0u ? 0u : rbvhDepthMipLevelOffset(atlasSize, rootMipLevel);
  return (rootMipOffset << 5u) | rootMipLevel;
}

static void assignCardDepthMipOffsets(RBVHBuildResult &result)
{
  for (RBVHCardPrimitive &card : result.cards)
  {
    card._padding0 = packRbvhCardMipMetadata(result.atlasSize, std::max(card.atlasWidth, card.atlasHeight));
  }
}

static uint32_t clampCardResolution(const uint32_t value, const RBVHBuildOptions &options)
{
  const uint32_t minResolution = std::max(options.minCardResolution, 1u);
  const uint32_t maxResolution = std::max(options.maxCardResolution, minResolution);
  return std::clamp(value, minResolution, maxResolution);
}

static float edge2(const float ax, const float ay, const float bx, const float by, const float px, const float py)
{
  return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static bool fetchTriangle(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex, const virtualgeometry::FallbackMeshVertex *outVertices[3])
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return false;
  }

  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return false;
    }
    outVertices[corner] = &mesh.vertices[vertexIndex];
  }
  return true;
}

static bool isValidAabb(const virtualgeometry::AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] &&
         bounds.minPoint[1] <= bounds.maxPoint[1] &&
         bounds.minPoint[2] <= bounds.maxPoint[2];
}

static bool aabbIntersects(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  if (!isValidAabb(a) || !isValidAabb(b))
  {
    return false;
  }

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (a.maxPoint[axis] < b.minPoint[axis] || b.maxPoint[axis] < a.minPoint[axis])
    {
      return false;
    }
  }
  return true;
}

static bool aabbContains(const virtualgeometry::AABB &outer, const virtualgeometry::AABB &inner)
{
  if (!isValidAabb(outer) || !isValidAabb(inner))
  {
    return false;
  }

  constexpr float kContainEpsilon = 1.0e-6f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (inner.minPoint[axis] < outer.minPoint[axis] - kContainEpsilon ||
        inner.maxPoint[axis] > outer.maxPoint[axis] + kContainEpsilon)
    {
      return false;
    }
  }
  return true;
}

static virtualgeometry::AABB unionBounds(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  return virtualgeometry::AABB::Union(a, b);
}

static virtualgeometry::AABB computeTriangleBounds(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const virtualgeometry::FallbackMeshVertex *vertices[3]{};
  if (!fetchTriangle(mesh, primitiveIndex, vertices))
  {
    return virtualgeometry::AABB{};
  }

  virtualgeometry::AABB bounds{};
  for (const virtualgeometry::FallbackMeshVertex *vertex : vertices)
  {
    bounds.expandBy(vertex->position);
  }
  return bounds;
}

static void computeCentroid(const virtualgeometry::AABB &bounds, float outCentroid[3])
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outCentroid[axis] = (bounds.minPoint[axis] + bounds.maxPoint[axis]) * 0.5f;
  }
}

struct PositionKey
{
  int64_t x = 0;
  int64_t y = 0;
  int64_t z = 0;

  bool operator==(const PositionKey &other) const
  {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct PositionKeyHash
{
  size_t operator()(const PositionKey &key) const
  {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const int64_t value)
    {
      hash ^= static_cast<uint64_t>(value);
      hash *= 1099511628211ull;
    };
    mix(key.x);
    mix(key.y);
    mix(key.z);
    return static_cast<size_t>(hash);
  }
};

struct EdgeKey
{
  PositionKey a{};
  PositionKey b{};

  bool operator==(const EdgeKey &other) const
  {
    return a == other.a && b == other.b;
  }
};

struct EdgeKeyHash
{
  size_t operator()(const EdgeKey &key) const
  {
    PositionKeyHash hashPosition;
    return hashPosition(key.a) ^ (hashPosition(key.b) + 0x9e3779b97f4a7c15ull + (hashPosition(key.a) << 6u) + (hashPosition(key.a) >> 2u));
  }
};

static PositionKey makePositionKey(const float position[3])
{
  constexpr double kScale = 100000.0;
  return PositionKey{
    .x = static_cast<int64_t>(std::llround(static_cast<double>(position[0]) * kScale)),
    .y = static_cast<int64_t>(std::llround(static_cast<double>(position[1]) * kScale)),
    .z = static_cast<int64_t>(std::llround(static_cast<double>(position[2]) * kScale)),
  };
}

static bool positionKeyLess(const PositionKey &a, const PositionKey &b)
{
  if (a.x != b.x)
    return a.x < b.x;
  if (a.y != b.y)
    return a.y < b.y;
  return a.z < b.z;
}

static EdgeKey makeEdgeKey(const PositionKey &a, const PositionKey &b)
{
  return positionKeyLess(b, a) ? EdgeKey{.a = b, .b = a} : EdgeKey{.a = a, .b = b};
}

static bool computeTriangleNormal(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex, float outNormal[3], float &outArea2)
{
  const virtualgeometry::FallbackMeshVertex *vertices[3]{};
  if (!fetchTriangle(mesh, primitiveIndex, vertices))
  {
    return false;
  }

  float e0[3]{};
  float e1[3]{};
  sub3(vertices[1]->position, vertices[0]->position, e0);
  sub3(vertices[2]->position, vertices[0]->position, e1);
  cross3(e0, e1, outNormal);
  outArea2 = length3(outNormal);
  return normalize3(outNormal);
}

static float computePolygonArea(const RBVHBuildPrimitive &primitive)
{
  if (primitive.vertexCount < 3u)
  {
    return 0.0f;
  }

  float area = 0.0f;
  for (uint32_t vertexIndex = 1u; vertexIndex + 1u < primitive.vertexCount; ++vertexIndex)
  {
    float edge0[3]{};
    float edge1[3]{};
    float cross[3]{};
    sub3(primitive.vertices[vertexIndex], primitive.vertices[0u], edge0);
    sub3(primitive.vertices[vertexIndex + 1u], primitive.vertices[0u], edge1);
    cross3(edge0, edge1, cross);
    area += primitive.hasNormal
        ? std::fabs(dot3(cross, primitive.normal)) * 0.5f
        : length3(cross) * 0.5f;
  }
  return area;
}

static bool updatePrimitiveGeometry(RBVHBuildPrimitive &primitive)
{
  if (primitive.vertexCount < 3u)
  {
    return false;
  }

  virtualgeometry::AABB bounds{};
  for (uint32_t vertexIndex = 0u; vertexIndex < primitive.vertexCount; ++vertexIndex)
  {
    bounds.expandBy(primitive.vertices[vertexIndex]);
  }

  primitive.bounds = bounds;
  computeCentroid(bounds, primitive.centroid);
  primitive.area = computePolygonArea(primitive);
  return primitive.area > 1.0e-12f && isValidAabb(primitive.bounds);
}

static void appendClippedVertex(float vertices[kMaxClippedPrimitiveVertices][3], uint32_t &count, const float vertex[3])
{
  if (count >= kMaxClippedPrimitiveVertices)
  {
    return;
  }
  copy3(vertex, vertices[count]);
  ++count;
}

static void intersectAxisPlane(const float a[3], const float b[3], const uint32_t axis, const float plane, float out[3])
{
  const float delta = b[axis] - a[axis];
  const float t = std::fabs(delta) > 1.0e-12f ? std::clamp((plane - a[axis]) / delta, 0.0f, 1.0f) : 0.0f;
  for (uint32_t component = 0u; component < 3u; ++component)
  {
    out[component] = a[component] + (b[component] - a[component]) * t;
  }
  out[axis] = plane;
}

static bool clipVerticesAgainstAxisPlane(
    const float input[kMaxClippedPrimitiveVertices][3],
    const uint32_t inputCount,
    float output[kMaxClippedPrimitiveVertices][3],
    uint32_t &outputCount,
    const uint32_t axis,
    const float plane,
    const bool keepGreater)
{
  outputCount = 0u;
  if (inputCount == 0u)
  {
    return false;
  }

  constexpr float kClipEpsilon = 1.0e-6f;
  const auto inside = [axis, plane, keepGreater](const float vertex[3])
  {
    return keepGreater ? vertex[axis] >= plane - kClipEpsilon : vertex[axis] <= plane + kClipEpsilon;
  };

  const float *previous = input[inputCount - 1u];
  bool previousInside = inside(previous);
  for (uint32_t vertexIndex = 0u; vertexIndex < inputCount; ++vertexIndex)
  {
    const float *current = input[vertexIndex];
    const bool currentInside = inside(current);
    if (currentInside != previousInside)
    {
      float intersection[3]{};
      intersectAxisPlane(previous, current, axis, plane, intersection);
      appendClippedVertex(output, outputCount, intersection);
    }
    if (currentInside)
    {
      appendClippedVertex(output, outputCount, current);
    }
    previous = current;
    previousInside = currentInside;
  }

  return outputCount >= 3u;
}

static bool clipPrimitiveToBounds(const RBVHBuildPrimitive &inputPrimitive, const virtualgeometry::AABB &clipBounds, RBVHBuildPrimitive &outputPrimitive)
{
  if (!isValidAabb(clipBounds) || inputPrimitive.vertexCount < 3u)
  {
    return false;
  }

  float input[kMaxClippedPrimitiveVertices][3]{};
  float output[kMaxClippedPrimitiveVertices][3]{};
  uint32_t inputCount = inputPrimitive.vertexCount;
  for (uint32_t vertexIndex = 0u; vertexIndex < inputCount; ++vertexIndex)
  {
    copy3(inputPrimitive.vertices[vertexIndex], input[vertexIndex]);
  }

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    uint32_t outputCount = 0u;
    if (!clipVerticesAgainstAxisPlane(input, inputCount, output, outputCount, axis, clipBounds.minPoint[axis], true))
    {
      return false;
    }
    inputCount = outputCount;
    for (uint32_t vertexIndex = 0u; vertexIndex < inputCount; ++vertexIndex)
    {
      copy3(output[vertexIndex], input[vertexIndex]);
    }

    if (!clipVerticesAgainstAxisPlane(input, inputCount, output, outputCount, axis, clipBounds.maxPoint[axis], false))
    {
      return false;
    }
    inputCount = outputCount;
    for (uint32_t vertexIndex = 0u; vertexIndex < inputCount; ++vertexIndex)
    {
      copy3(output[vertexIndex], input[vertexIndex]);
    }
  }

  outputPrimitive = inputPrimitive;
  outputPrimitive.vertexCount = inputCount;
  for (uint32_t vertexIndex = 0u; vertexIndex < inputCount; ++vertexIndex)
  {
    copy3(input[vertexIndex], outputPrimitive.vertices[vertexIndex]);
  }
  return updatePrimitiveGeometry(outputPrimitive);
}

static bool slerpUnit3(const float from[3], const float to[3], const float t, float out[3])
{
  const float cosTheta = clampUnit(dot3(from, to));
  const float theta = std::acos(cosTheta);
  if (theta <= 1.0e-6f)
  {
    copy3(from, out);
    return true;
  }

  const float sinTheta = std::sin(theta);
  if (std::fabs(sinTheta) <= 1.0e-6f)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      out[axis] = from[axis] * (1.0f - t) + to[axis] * t;
    }
    return normalize3(out);
  }

  const float fromWeight = std::sin((1.0f - t) * theta) / sinTheta;
  const float toWeight = std::sin(t * theta) / sinTheta;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    out[axis] = from[axis] * fromWeight + to[axis] * toWeight;
  }
  return normalize3(out);
}

static bool dominantNormalAxis(const float normalMoment[3][3], float outAxis[3])
{
  uint32_t startAxis = 0u;
  if (normalMoment[1][1] > normalMoment[startAxis][startAxis])
  {
    startAxis = 1u;
  }
  if (normalMoment[2][2] > normalMoment[startAxis][startAxis])
  {
    startAxis = 2u;
  }

  outAxis[0] = startAxis == 0u ? 1.0f : 0.0f;
  outAxis[1] = startAxis == 1u ? 1.0f : 0.0f;
  outAxis[2] = startAxis == 2u ? 1.0f : 0.0f;

  for (uint32_t iteration = 0u; iteration < 12u; ++iteration)
  {
    float next[3] = {
      normalMoment[0][0] * outAxis[0] + normalMoment[0][1] * outAxis[1] + normalMoment[0][2] * outAxis[2],
      normalMoment[1][0] * outAxis[0] + normalMoment[1][1] * outAxis[1] + normalMoment[1][2] * outAxis[2],
      normalMoment[2][0] * outAxis[0] + normalMoment[2][1] * outAxis[1] + normalMoment[2][2] * outAxis[2],
    };
    if (!normalize3(next))
    {
      return false;
    }
    copy3(next, outAxis);
  }

  uint32_t largestAxis = 0u;
  if (std::fabs(outAxis[1]) > std::fabs(outAxis[largestAxis]))
  {
    largestAxis = 1u;
  }
  if (std::fabs(outAxis[2]) > std::fabs(outAxis[largestAxis]))
  {
    largestAxis = 2u;
  }
  if (outAxis[largestAxis] < 0.0f)
  {
    outAxis[0] = -outAxis[0];
    outAxis[1] = -outAxis[1];
    outAxis[2] = -outAxis[2];
  }
  return true;
}

static void buildCardFrameFromNormal(const float axisN[3], float axisU[3], float axisV[3])
{
  const float upY[3] = {0.0f, 1.0f, 0.0f};
  const float upX[3] = {1.0f, 0.0f, 0.0f};
  cross3(std::fabs(axisN[1]) < 0.9f ? upY : upX, axisN, axisU);
  if (!normalize3(axisU))
  {
    axisU[0] = 1.0f;
    axisU[1] = 0.0f;
    axisU[2] = 0.0f;
  }
  cross3(axisN, axisU, axisV);
  if (!normalize3(axisV))
  {
    axisV[0] = 0.0f;
    axisV[1] = 1.0f;
    axisV[2] = 0.0f;
  }
}

static void fitCardFrameToBounds(const virtualgeometry::AABB &bounds, const float axisN[3], float axisU[3], float axisV[3])
{
  if (!isValidAabb(bounds))
  {
    return;
  }

  float points[8][2]{};
  for (uint32_t cornerMask = 0u; cornerMask < 8u; ++cornerMask)
  {
    float corner[3]{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      corner[axis] = (cornerMask & (1u << axis)) != 0u ? bounds.maxPoint[axis] : bounds.minPoint[axis];
    }
    points[cornerMask][0] = dot3(corner, axisU);
    points[cornerMask][1] = dot3(corner, axisV);
  }

  float bestAxisX = 1.0f;
  float bestAxisY = 0.0f;
  float bestArea = std::numeric_limits<float>::max();
  const auto evaluateCandidate = [&](const float candidateX, const float candidateY)
  {
    const float length = std::sqrt(candidateX * candidateX + candidateY * candidateY);
    if (!(length > 1.0e-8f))
    {
      return;
    }
    const float ux = candidateX / length;
    const float uy = candidateY / length;
    const float vx = -uy;
    const float vy = ux;
    float minU = std::numeric_limits<float>::max();
    float minV = std::numeric_limits<float>::max();
    float maxU = std::numeric_limits<float>::lowest();
    float maxV = std::numeric_limits<float>::lowest();
    for (const auto &point : points)
    {
      const float u = point[0] * ux + point[1] * uy;
      const float v = point[0] * vx + point[1] * vy;
      minU = std::min(minU, u);
      minV = std::min(minV, v);
      maxU = std::max(maxU, u);
      maxV = std::max(maxV, v);
    }
    const float area = std::max(maxU - minU, 0.0f) * std::max(maxV - minV, 0.0f);
    if (area < bestArea)
    {
      bestArea = area;
      bestAxisX = ux;
      bestAxisY = uy;
    }
  };

  evaluateCandidate(1.0f, 0.0f);
  for (uint32_t a = 0u; a < 8u; ++a)
  {
    for (uint32_t b = a + 1u; b < 8u; ++b)
    {
      evaluateCandidate(points[b][0] - points[a][0], points[b][1] - points[a][1]);
    }
  }

  float newAxisU[3]{};
  float newAxisV[3]{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    newAxisU[axis] = axisU[axis] * bestAxisX + axisV[axis] * bestAxisY;
    newAxisV[axis] = axisU[axis] * -bestAxisY + axisV[axis] * bestAxisX;
  }
  if (normalize3(newAxisU) && normalize3(newAxisV))
  {
    copy3(newAxisU, axisU);
    copy3(newAxisV, axisV);
  }
}

struct SurfaceStats
{
  bool valid = false;
  float axisN[3] = {0.0f, 0.0f, 1.0f};
  float averageAxis[3] = {0.0f, 0.0f, 1.0f};
  float coneAxis[3] = {0.0f, 0.0f, 1.0f};
  float weightedNormal[3] = {0.0f, 0.0f, 0.0f};
  float surfaceArea = 0.0f;
  float projectedArea = 0.0f;
  float projectedAreaRatio = 0.0f;
  float minNormalDot = 1.0f;
  float depthToProjectedExtent = std::numeric_limits<float>::max();
};

struct CardResolution
{
  uint32_t width = 1u;
  uint32_t height = 1u;
};

static float estimatePaperTileResolution(const SurfaceStats &stats, const RBVHBuildOptions &options)
{
  const float rho = std::max(options.pixelToAreaRatio, 1.0f);
  const float surfaceArea = std::max(stats.surfaceArea, 1.0e-8f);
  const float projectedArea = std::max(stats.projectedArea, surfaceArea * 1.0e-4f);
  return std::sqrt(std::max(rho * surfaceArea * surfaceArea / projectedArea, 1.0f));
}

static CardResolution requestedCardResolution(
    const RBVHBuildOptions &options,
    const SurfaceStats &stats)
{
  const float resolution = estimatePaperTileResolution(stats, options);
  const uint32_t squareResolution = clampCardResolution(static_cast<uint32_t>(std::ceil(resolution)), options);
  return CardResolution{
    .width = squareResolution,
    .height = squareResolution,
  };
}

static float estimatePaperCoherentTargetTileResolution(const float pixelToAreaRatio, const RBVHBuildOptions &options)
{
  const float rho = std::max(pixelToAreaRatio, 1.0f);
  const float target = 5.76f * std::log(rho) - 42.99f;
  return std::clamp(target, static_cast<float>(options.minCardResolution), static_cast<float>(options.maxCardResolution));
}

static bool approximateNormalCone(
    const std::vector<RBVHBuildPrimitive> &primitives,
    const uint32_t begin,
    const uint32_t end,
    float outAxis[3],
    float &outMinNormalDot)
{
  bool hasCone = false;
  float coneAxis[3] = {0.0f, 0.0f, 1.0f};
  float coneCos = 1.0f;

  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (!primitive.hasNormal)
    {
      continue;
    }

    if (!hasCone)
    {
      copy3(primitive.normal, coneAxis);
      hasCone = true;
      continue;
    }

    const float dot = clampUnit(dot3(coneAxis, primitive.normal));
    if (dot >= coneCos)
    {
      continue;
    }

    const float oldAngle = std::acos(clampUnit(coneCos));
    const float normalAngle = std::acos(dot);
    const float moveAngle = (normalAngle - oldAngle) * 0.5f;
    const float newAngle = oldAngle + moveAngle;
    float newAxis[3]{};
    if (!slerpUnit3(coneAxis, primitive.normal, normalAngle > 1.0e-6f ? moveAngle / normalAngle : 0.0f, newAxis))
    {
      return false;
    }
    copy3(newAxis, coneAxis);
    coneCos = std::cos(newAngle);
  }

  if (!hasCone)
  {
    return false;
  }

  outMinNormalDot = 1.0f;
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (!primitive.hasNormal)
    {
      continue;
    }
    outMinNormalDot = std::min(outMinNormalDot, dot3(coneAxis, primitive.normal));
  }

  copy3(coneAxis, outAxis);
  return true;
}

struct RBVHNodeStats
{
  virtualgeometry::AABB bounds{};
  virtualgeometry::AABB centroidBounds{};
  SurfaceStats surface{};
  uint32_t firstComponentId = 0u;
  uint32_t componentCount = 0u;
  bool hasBounds = false;
};

static RBVHBuildOptions adaptiveOptionsForNode(const RBVHBuildOptions &baseOptions, const RBVHNodeStats &nodeStats)
{
  if (!baseOptions.adaptiveLodEnabled || !nodeStats.hasBounds)
  {
    return baseOptions;
  }

  float center[3]{};
  computeCentroid(nodeStats.bounds, center);
  float delta[3]{};
  sub3(center, baseOptions.adaptiveLodPoint, delta);
  const float distance = length3(delta);
  const float nearDistance = std::max(baseOptions.adaptiveLodNearDistance, 0.0f);
  const float farDistance = std::max(baseOptions.adaptiveLodFarDistance, nearDistance + 1.0e-4f);
  const float t = std::clamp((distance - nearDistance) / (farDistance - nearDistance), 0.0f, 1.0f);

  RBVHBuildOptions options = baseOptions;
  options.minNormalDot = baseOptions.minNormalDot + (baseOptions.adaptiveLodFarMinNormalDot - baseOptions.minNormalDot) * t;
  options.projectedAreaThreshold = baseOptions.projectedAreaThreshold + (baseOptions.adaptiveLodFarProjectedAreaThreshold - baseOptions.projectedAreaThreshold) * t;
  const float pixelScale = 1.0f + (std::max(baseOptions.adaptiveLodFarPixelToAreaScale, 1.0e-4f) - 1.0f) * t;
  options.pixelToAreaRatio = std::max(baseOptions.pixelToAreaRatio * pixelScale, 1.0f);
  if (baseOptions.targetTileResolution > 0.0f)
  {
    const float targetScale = 1.0f + (std::max(baseOptions.adaptiveLodFarTargetTileResolutionScale, 1.0e-4f) - 1.0f) * t;
    options.targetTileResolution = std::max(baseOptions.targetTileResolution * targetScale, static_cast<float>(options.minCardResolution));
  }
  return options;
}

static SurfaceStats selectSurfaceProjection(const SurfaceStats &stats, const RBVHRefinementCriterion criterion)
{
  SurfaceStats selected = stats;
  if (criterion == RBVHRefinementCriterion::ProjectedArea)
  {
    copy3(stats.averageAxis, selected.axisN);
  }
  else
  {
    copy3(stats.coneAxis, selected.axisN);
  }
  return selected;
}

struct RBVHSplit
{
  bool valid = false;
  uint32_t axis = 0u;
  uint32_t binSplit = 0u;
  float planePosition = 0.0f;
  float cost = std::numeric_limits<float>::max();
  bool componentSplit = false;
  bool spatialMedianSplit = false;
  std::vector<uint32_t> leftComponentIds;
};

struct RBVHSahBin
{
  virtualgeometry::AABB bounds{};
  float surfaceArea = 0.0f;
  uint32_t count = 0u;
  bool hasBounds = false;
};

class DisjointSet
{
public:
  explicit DisjointSet(const uint32_t count) : parents_(count), ranks_(count, 0u)
  {
    for (uint32_t index = 0u; index < count; ++index)
    {
      parents_[index] = index;
    }
  }

  uint32_t find(const uint32_t value)
  {
    uint32_t parent = parents_[value];
    if (parent != value)
    {
      parent = find(parent);
      parents_[value] = parent;
    }
    return parent;
  }

  void unite(const uint32_t a, const uint32_t b)
  {
    uint32_t rootA = find(a);
    uint32_t rootB = find(b);
    if (rootA == rootB)
    {
      return;
    }
    if (ranks_[rootA] < ranks_[rootB])
    {
      std::swap(rootA, rootB);
    }
    parents_[rootB] = rootA;
    if (ranks_[rootA] == ranks_[rootB])
    {
      ++ranks_[rootA];
    }
  }

private:
  std::vector<uint32_t> parents_;
  std::vector<uint8_t> ranks_;
};

static std::vector<uint32_t> computeTriangleComponents(const virtualgeometry::FallbackMeshData &mesh)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  std::vector<uint32_t> components(primitiveCount, 0u);
  if (primitiveCount == 0u)
  {
    return components;
  }

  DisjointSet disjointSet(primitiveCount);

  std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash> firstPrimitiveForEdge;
  firstPrimitiveForEdge.reserve(primitiveCount * 3u);
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    const virtualgeometry::FallbackMeshVertex *vertices[3]{};
    if (!fetchTriangle(mesh, primitiveIndex, vertices))
    {
      continue;
    }

    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      const uint32_t cornerA = edge;
      const uint32_t cornerB = (edge + 1u) % 3u;
      const PositionKey positionA = makePositionKey(vertices[cornerA]->position);
      const PositionKey positionB = makePositionKey(vertices[cornerB]->position);
      const EdgeKey key = makeEdgeKey(positionA, positionB);

      auto [it, inserted] = firstPrimitiveForEdge.emplace(key, primitiveIndex);
      if (!inserted)
      {
        disjointSet.unite(it->second, primitiveIndex);
      }
    }
  }

  std::unordered_map<uint32_t, uint32_t> compactIds;
  compactIds.reserve(primitiveCount);
  uint32_t nextComponentId = 0u;
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    const uint32_t root = disjointSet.find(primitiveIndex);
    auto [it, inserted] = compactIds.emplace(root, nextComponentId);
    if (inserted)
    {
      ++nextComponentId;
    }
    components[primitiveIndex] = it->second;
  }
  return components;
}

static std::vector<CWBVHBuilder::PrimitiveBounds> buildPrimitiveBounds(const virtualgeometry::FallbackMeshData &mesh)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  std::vector<CWBVHBuilder::PrimitiveBounds> primitiveBounds;
  primitiveBounds.reserve(primitiveCount);
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    const virtualgeometry::AABB bounds = computeTriangleBounds(mesh, primitiveIndex);
    if (!isValidAabb(bounds))
    {
      continue;
    }
    primitiveBounds.push_back(
        CWBVHBuilder::PrimitiveBounds{
          .bounds = bounds,
          .primitiveIndex = primitiveIndex,
        });
  }
  return primitiveBounds;
}

static std::vector<CWBVHBuilder::PrimitiveBounds> buildPrimitiveBoundsFromTriangleBuild(const CWBVHBuilder::BuildResult &triangleBuild)
{
  std::vector<CWBVHBuilder::PrimitiveBounds> primitiveBounds;
  if (triangleBuild.primitiveRemap.empty() || triangleBuild.canonicalNodes.empty())
  {
    return primitiveBounds;
  }

  primitiveBounds.reserve(triangleBuild.primitiveRemap.size());
  for (const CWBVHBuilder::CanonicalNode &node : triangleBuild.canonicalNodes)
  {
    if (!node.isLeaf || node.primitiveIndex >= triangleBuild.primitiveRemap.size() || !isValidAabb(node.bounds))
    {
      continue;
    }
    primitiveBounds.push_back(
        CWBVHBuilder::PrimitiveBounds{
          .bounds = node.bounds,
          .primitiveIndex = triangleBuild.primitiveRemap[node.primitiveIndex],
        });
  }
  return primitiveBounds;
}

static std::vector<RBVHBuildPrimitive> buildPrimitiveRefs(
    const virtualgeometry::FallbackMeshData &mesh,
    const std::vector<CWBVHBuilder::PrimitiveBounds> &inputPrimitiveBounds)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  const std::vector<uint32_t> triangleComponents = computeTriangleComponents(mesh);
  std::vector<virtualgeometry::AABB> boundsByPrimitive(primitiveCount);
  std::vector<uint8_t> hasInputBounds(primitiveCount, 0u);
  for (const CWBVHBuilder::PrimitiveBounds &input : inputPrimitiveBounds)
  {
    if (input.primitiveIndex >= primitiveCount || !isValidAabb(input.bounds))
    {
      continue;
    }
    boundsByPrimitive[input.primitiveIndex] = input.bounds;
    hasInputBounds[input.primitiveIndex] = 1u;
  }

  std::vector<RBVHBuildPrimitive> primitives;
  primitives.reserve(primitiveCount);

  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    const virtualgeometry::AABB primitiveBounds = hasInputBounds[primitiveIndex] != 0u
        ? boundsByPrimitive[primitiveIndex]
        : computeTriangleBounds(mesh, primitiveIndex);
    if (!isValidAabb(primitiveBounds))
    {
      continue;
    }

    RBVHBuildPrimitive primitive{};
    primitive.primitiveIndex = primitiveIndex;
    primitive.componentId = primitiveIndex < triangleComponents.size() ? triangleComponents[primitiveIndex] : primitiveIndex;
    primitive.bounds = primitiveBounds;
    computeCentroid(primitiveBounds, primitive.centroid);

    float area2 = 0.0f;
    primitive.hasNormal = computeTriangleNormal(mesh, primitiveIndex, primitive.normal, area2);
    primitive.area = primitive.hasNormal ? area2 * 0.5f : 0.0f;
    const virtualgeometry::FallbackMeshVertex *vertices[3]{};
    if (fetchTriangle(mesh, primitiveIndex, vertices))
    {
      primitive.vertexCount = 3u;
      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        copy3(vertices[corner]->position, primitive.vertices[corner]);
      }
    }
    primitives.push_back(primitive);
  }

  return primitives;
}

static std::vector<RBVHBuildPrimitive> buildPrimitiveRefs(const virtualgeometry::FallbackMeshData &mesh)
{
  return buildPrimitiveRefs(mesh, buildPrimitiveBounds(mesh));
}

static RBVHNodeStats computeNodeStats(const std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end)
{
  RBVHNodeStats stats{};
  if (begin >= end || end > primitives.size())
  {
    return stats;
  }

  std::unordered_set<uint32_t> componentIds;
  componentIds.reserve(end - begin);
  float normalMoment[3][3]{};

  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    stats.bounds = stats.hasBounds ? unionBounds(stats.bounds, primitive.bounds) : primitive.bounds;
    stats.hasBounds = true;

    if (index == begin)
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        stats.centroidBounds.minPoint[axis] = primitive.centroid[axis];
        stats.centroidBounds.maxPoint[axis] = primitive.centroid[axis];
      }
      stats.firstComponentId = primitive.componentId;
    }
    else
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        stats.centroidBounds.minPoint[axis] = std::min(stats.centroidBounds.minPoint[axis], primitive.centroid[axis]);
        stats.centroidBounds.maxPoint[axis] = std::max(stats.centroidBounds.maxPoint[axis], primitive.centroid[axis]);
      }
    }
    componentIds.insert(primitive.componentId);

    if (primitive.hasNormal && primitive.area > 0.0f)
    {
      stats.surface.surfaceArea += primitive.area;
      stats.surface.weightedNormal[0] += primitive.normal[0] * primitive.area;
      stats.surface.weightedNormal[1] += primitive.normal[1] * primitive.area;
      stats.surface.weightedNormal[2] += primitive.normal[2] * primitive.area;
      for (uint32_t row = 0u; row < 3u; ++row)
      {
        for (uint32_t column = 0u; column < 3u; ++column)
        {
          normalMoment[row][column] += primitive.normal[row] * primitive.normal[column] * primitive.area;
        }
      }
    }
  }

  stats.componentCount = static_cast<uint32_t>(componentIds.size());
  float summedAreaNormal[3] = {
    stats.surface.weightedNormal[0],
    stats.surface.weightedNormal[1],
    stats.surface.weightedNormal[2],
  };
  stats.surface.projectedArea = length3(summedAreaNormal);
  stats.surface.projectedAreaRatio = stats.surface.surfaceArea > 1.0e-12f ? stats.surface.projectedArea / stats.surface.surfaceArea : 0.0f;

  float averageAxis[3] = {
    summedAreaNormal[0],
    summedAreaNormal[1],
    summedAreaNormal[2],
  };
  const bool hasAverageAxis = normalize3(averageAxis);
  if (hasAverageAxis)
  {
    copy3(averageAxis, stats.surface.averageAxis);
  }
  else
  {
    dominantNormalAxis(normalMoment, stats.surface.averageAxis);
  }

  float coneMinNormalDot = 0.0f;
  const bool hasConeAxis = approximateNormalCone(primitives, begin, end, stats.surface.coneAxis, coneMinNormalDot);
  if (!hasConeAxis)
  {
    copy3(stats.surface.averageAxis, stats.surface.coneAxis);
    coneMinNormalDot = hasAverageAxis ? 1.0f : 0.0f;
  }
  stats.surface.valid = hasAverageAxis || hasConeAxis;
  copy3(hasConeAxis ? stats.surface.coneAxis : stats.surface.averageAxis, stats.surface.axisN);
  if (stats.surface.valid && dot3(stats.surface.axisN, stats.surface.weightedNormal) < 0.0f)
  {
    stats.surface.axisN[0] = -stats.surface.axisN[0];
    stats.surface.axisN[1] = -stats.surface.axisN[1];
    stats.surface.axisN[2] = -stats.surface.axisN[2];
  }
  if (!stats.surface.valid)
  {
    stats.surface.axisN[0] = 0.0f;
    stats.surface.axisN[1] = 0.0f;
    stats.surface.axisN[2] = 1.0f;
  }

  bool hasNormal = false;
  stats.surface.minNormalDot = coneMinNormalDot;
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (!primitive.hasNormal)
    {
      continue;
    }
    hasNormal = true;
  }
  if (stats.surface.valid)
  {
    float axisU[3]{};
    float axisV[3]{};
    buildCardFrameFromNormal(stats.surface.axisN, axisU, axisV);
    float minU = std::numeric_limits<float>::max();
    float minV = std::numeric_limits<float>::max();
    float minN = std::numeric_limits<float>::max();
    float maxU = std::numeric_limits<float>::lowest();
    float maxV = std::numeric_limits<float>::lowest();
    float maxN = std::numeric_limits<float>::lowest();

    for (uint32_t index = begin; index < end; ++index)
    {
      const RBVHBuildPrimitive &primitive = primitives[index];
      for (uint32_t corner = 0u; corner < primitive.vertexCount; ++corner)
      {
        const float u = dot3(primitive.vertices[corner], axisU);
        const float v = dot3(primitive.vertices[corner], axisV);
        const float n = dot3(primitive.vertices[corner], stats.surface.axisN);
        minU = std::min(minU, u);
        minV = std::min(minV, v);
        minN = std::min(minN, n);
        maxU = std::max(maxU, u);
        maxV = std::max(maxV, v);
        maxN = std::max(maxN, n);
      }
    }

    const float projectedExtent = std::max(maxU - minU, maxV - minV);
    const float depthRange = maxN - minN;
    stats.surface.depthToProjectedExtent = projectedExtent > 1.0e-8f ? depthRange / projectedExtent : std::numeric_limits<float>::max();
  }
  if (!hasNormal)
  {
    stats.surface.projectedArea = 0.0f;
    stats.surface.projectedAreaRatio = 0.0f;
    stats.surface.minNormalDot = 0.0f;
  }

  return stats;
}

static RBVHSplit findComponentSplit(const std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const RBVHNodeStats &nodeStats)
{
  RBVHSplit split{};
  if (nodeStats.componentCount != 2u || begin + 1u >= end)
  {
    return split;
  }

  struct ComponentAccum
  {
    uint32_t componentId = 0u;
    float centroid[3] = {0.0f, 0.0f, 0.0f};
    float surfaceArea = 0.0f;
    uint32_t count = 0u;
  };

  std::vector<ComponentAccum> components;
  components.reserve(nodeStats.componentCount);
  std::unordered_map<uint32_t, size_t> componentLookup;
  componentLookup.reserve(nodeStats.componentCount);
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    auto [lookupIt, inserted] = componentLookup.emplace(primitive.componentId, components.size());
    if (inserted)
    {
      ComponentAccum component{};
      component.componentId = primitive.componentId;
      components.push_back(component);
    }
    ComponentAccum &component = components[lookupIt->second];

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      component.centroid[axis] += primitive.centroid[axis];
    }
    component.surfaceArea += primitive.area;
    ++component.count;
  }

  for (ComponentAccum &component : components)
  {
    const float invCount = component.count > 0u ? 1.0f / static_cast<float>(component.count) : 0.0f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      component.centroid[axis] *= invCount;
    }
  }

  split.valid = true;
  split.componentSplit = true;
  split.cost = 0.0f;
  split.leftComponentIds.push_back(components[0u].componentId);
  return split;
}

static float rbvhSplitCost(
    const virtualgeometry::AABB &parentBounds,
    const virtualgeometry::AABB &leftBounds,
    const virtualgeometry::AABB &rightBounds,
    const float leftSurfaceArea,
    const float rightSurfaceArea)
{
  const float parentArea = std::max(parentBounds.GetSurfaceArea(), 1.0e-8f);
  const float leftProbability = std::clamp(leftBounds.GetSurfaceArea() / parentArea, 0.0f, 1.0f);
  const float rightProbability = std::clamp(rightBounds.GetSurfaceArea() / parentArea, 0.0f, 1.0f);
  return leftProbability * leftSurfaceArea + rightProbability * rightSurfaceArea;
}

static RBVHSplit findSahSplit(const std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const RBVHNodeStats &nodeStats, const RBVHBuildOptions &options)
{
  RBVHSplit best{};
  if (begin + 1u >= end || !nodeStats.hasBounds)
  {
    return best;
  }

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float minCentroid = nodeStats.centroidBounds.minPoint[axis];
    const float maxCentroid = nodeStats.centroidBounds.maxPoint[axis];
    const float extent = maxCentroid - minCentroid;
    if (extent <= 1.0e-6f)
    {
      continue;
    }

    std::vector<RBVHSahBin> bins(options.sahBinCount);
    for (uint32_t index = begin; index < end; ++index)
    {
      const RBVHBuildPrimitive &primitive = primitives[index];
      const float normalized = (primitive.centroid[axis] - minCentroid) / extent;
      const uint32_t binIndex = std::min<uint32_t>(
          options.sahBinCount - 1u,
          static_cast<uint32_t>(normalized * static_cast<float>(options.sahBinCount)));
      RBVHSahBin &bin = bins[binIndex];
      bin.bounds = bin.hasBounds ? unionBounds(bin.bounds, primitive.bounds) : primitive.bounds;
      bin.surfaceArea += primitive.area;
      bin.count += 1u;
      bin.hasBounds = true;
    }

    std::vector<virtualgeometry::AABB> leftBounds(options.sahBinCount - 1u);
    std::vector<virtualgeometry::AABB> rightBounds(options.sahBinCount - 1u);
    std::vector<float> leftSurfaceAreas(options.sahBinCount - 1u, 0.0f);
    std::vector<float> rightSurfaceAreas(options.sahBinCount - 1u, 0.0f);
    std::vector<uint32_t> leftCounts(options.sahBinCount - 1u, 0u);
    std::vector<uint32_t> rightCounts(options.sahBinCount - 1u, 0u);

    virtualgeometry::AABB runningLeftBounds{};
    float runningLeftSurfaceArea = 0.0f;
    uint32_t runningLeftCount = 0u;
    bool hasRunningLeftBounds = false;
    for (uint32_t split = 0u; split < options.sahBinCount - 1u; ++split)
    {
      if (bins[split].hasBounds)
      {
        runningLeftBounds = hasRunningLeftBounds ? unionBounds(runningLeftBounds, bins[split].bounds) : bins[split].bounds;
        hasRunningLeftBounds = true;
      }
      runningLeftSurfaceArea += bins[split].surfaceArea;
      runningLeftCount += bins[split].count;
      leftBounds[split] = runningLeftBounds;
      leftSurfaceAreas[split] = runningLeftSurfaceArea;
      leftCounts[split] = runningLeftCount;
    }

    virtualgeometry::AABB runningRightBounds{};
    float runningRightSurfaceArea = 0.0f;
    uint32_t runningRightCount = 0u;
    bool hasRunningRightBounds = false;
    for (uint32_t split = options.sahBinCount - 1u; split > 0u; --split)
    {
      if (bins[split].hasBounds)
      {
        runningRightBounds = hasRunningRightBounds ? unionBounds(runningRightBounds, bins[split].bounds) : bins[split].bounds;
        hasRunningRightBounds = true;
      }
      runningRightSurfaceArea += bins[split].surfaceArea;
      runningRightCount += bins[split].count;
      rightBounds[split - 1u] = runningRightBounds;
      rightSurfaceAreas[split - 1u] = runningRightSurfaceArea;
      rightCounts[split - 1u] = runningRightCount;
    }

    for (uint32_t split = 0u; split < options.sahBinCount - 1u; ++split)
    {
      if (leftCounts[split] == 0u || rightCounts[split] == 0u)
      {
        continue;
      }

      const float cost = rbvhSplitCost(nodeStats.bounds, leftBounds[split], rightBounds[split], leftSurfaceAreas[split], rightSurfaceAreas[split]);
      if (cost < best.cost)
      {
        best.valid = true;
        best.axis = axis;
        best.binSplit = split;
        best.planePosition = minCentroid + extent * (static_cast<float>(split + 1u) / static_cast<float>(options.sahBinCount));
        best.cost = cost;
      }
    }
  }

  return best;
}

static RBVHSplit findSpatialMedianSplit(const uint32_t begin, const uint32_t end, const RBVHNodeStats &nodeStats)
{
  RBVHSplit split{};
  if (begin + 1u >= end || !nodeStats.hasBounds)
  {
    return split;
  }

  uint32_t splitAxis = 0u;
  float maxExtent = nodeStats.bounds.maxPoint[0] - nodeStats.bounds.minPoint[0];
  for (uint32_t axis = 1u; axis < 3u; ++axis)
  {
    const float extent = nodeStats.bounds.maxPoint[axis] - nodeStats.bounds.minPoint[axis];
    if (extent > maxExtent)
    {
      maxExtent = extent;
      splitAxis = axis;
    }
  }

  if (maxExtent <= 1.0e-6f)
  {
    return split;
  }

  split.valid = true;
  split.spatialMedianSplit = true;
  split.axis = splitAxis;
  split.planePosition = (nodeStats.bounds.minPoint[splitAxis] + nodeStats.bounds.maxPoint[splitAxis]) * 0.5f;
  split.cost = 0.0f;
  return split;
}

static RBVHCardPrimitive makeCard(
    const virtualgeometry::AABB &nodeBounds,
    const uint32_t sourceNodeIndex,
    const std::vector<RBVHBuildPrimitive> &primitives,
    const uint32_t begin,
    const uint32_t end,
    const uint32_t primitiveOffset,
    const RBVHBuildOptions &options,
    const SurfaceStats &stats)
{
  RBVHCardPrimitive card{};
  card.sourceNodeIndex = sourceNodeIndex;
  card.sourcePrimitiveOffset = primitiveOffset;
  card.sourcePrimitiveCount = end - begin;

  card.axisN[0] = stats.axisN[0];
  card.axisN[1] = stats.axisN[1];
  card.axisN[2] = stats.axisN[2];
  buildCardFrameFromNormal(card.axisN, card.axisU, card.axisV);

  float minU = std::numeric_limits<float>::max();
  float minV = std::numeric_limits<float>::max();
  float minN = std::numeric_limits<float>::max();
  float maxU = std::numeric_limits<float>::lowest();
  float maxV = std::numeric_limits<float>::lowest();
  float maxN = std::numeric_limits<float>::lowest();

  virtualgeometry::AABB primitiveBounds{};
  bool hasPrimitiveBounds = false;
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    for (uint32_t vertexIndex = 0u; vertexIndex < primitive.vertexCount; ++vertexIndex)
    {
      primitiveBounds.expandBy(primitive.vertices[vertexIndex]);
      hasPrimitiveBounds = true;
    }
  }

  if (!hasPrimitiveBounds)
  {
    return card;
  }

  const virtualgeometry::AABB cardFrustumBounds = isValidAabb(nodeBounds) ? nodeBounds : primitiveBounds;
  fitCardFrameToBounds(cardFrustumBounds, card.axisN, card.axisU, card.axisV);

  for (uint32_t cornerMask = 0u; cornerMask < 8u; ++cornerMask)
  {
    float corner[3]{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      corner[axis] = (cornerMask & (1u << axis)) != 0u ? cardFrustumBounds.maxPoint[axis] : cardFrustumBounds.minPoint[axis];
    }

    const float u = dot3(corner, card.axisU);
    const float v = dot3(corner, card.axisV);
    const float n = dot3(corner, card.axisN);
    minU = std::min(minU, u);
    minV = std::min(minV, v);
    minN = std::min(minN, n);
    maxU = std::max(maxU, u);
    maxV = std::max(maxV, v);
    maxN = std::max(maxN, n);
  }

  const float centerU = (minU + maxU) * 0.5f;
  const float centerV = (minV + maxV) * 0.5f;
  const float centerN = (minN + maxN) * 0.5f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    card.origin[axis] = card.axisU[axis] * centerU + card.axisV[axis] * centerV + card.axisN[axis] * centerN;
  }

  const float pad = std::max(options.cardBoundsPadding, 1.0e-6f);
  card.halfExtentU = std::max((maxU - minU) * 0.5f + pad, pad);
  card.halfExtentV = std::max((maxV - minV) * 0.5f + pad, pad);
  card.depthMin = minN - pad;
  card.depthMax = maxN + pad;
  const CardResolution resolution = requestedCardResolution(options, stats);
  card.atlasWidth = std::max(resolution.width, 1u);
  card.atlasHeight = std::max(resolution.height, 1u);
  const float samplingErrorU = card.halfExtentU * 2.0f / static_cast<float>(std::max(card.atlasWidth, 1u));
  const float samplingErrorV = card.halfExtentV * 2.0f / static_cast<float>(std::max(card.atlasHeight, 1u));
  card.samplingError = std::max(samplingErrorU, samplingErrorV);

  float primitiveMaxExtent = 0.0f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    primitiveMaxExtent = std::max(primitiveMaxExtent, primitiveBounds.maxPoint[axis] - primitiveBounds.minPoint[axis]);
  }

  const float depthQuantizationError = std::max(card.depthMax - card.depthMin, 0.0f) / 65535.0f;
  const float samplingBoundsPad = std::min(card.samplingError * 0.5f, std::max(primitiveMaxExtent * 0.01f, pad));
  const float metadataBoundsPad = pad + samplingBoundsPad + depthQuantizationError;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float axisPadding =
        metadataBoundsPad * (std::fabs(card.axisU[axis]) + std::fabs(card.axisV[axis]) + std::fabs(card.axisN[axis]));
    card.boundsMin[axis] = primitiveBounds.minPoint[axis] - axisPadding;
    card.boundsMax[axis] = primitiveBounds.maxPoint[axis] + axisPadding;
  }

  return card;
}

static virtualgeometry::AABB cardTraversalBounds(const RBVHCardPrimitive &card, const RBVHBuildOptions &options)
{
  (void)options;
  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = card.boundsMin[axis];
    bounds.maxPoint[axis] = card.boundsMax[axis];
  }
  return bounds;
}

static uint32_t partitionBySahSplit(std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const RBVHSplit &split, const RBVHNodeStats &nodeStats, const RBVHBuildOptions &options)
{
  const float centroidMin = nodeStats.centroidBounds.minPoint[split.axis];
  const float centroidMax = nodeStats.centroidBounds.maxPoint[split.axis];
  const float extent = centroidMax - centroidMin;
  auto midIt = std::partition(
      primitives.begin() + begin,
      primitives.begin() + end,
      [&](const RBVHBuildPrimitive &primitive)
      {
        const float normalized = extent <= 1.0e-6f ? 0.0f : (primitive.centroid[split.axis] - centroidMin) / extent;
        const uint32_t binIndex = std::min<uint32_t>(
            options.sahBinCount - 1u,
            static_cast<uint32_t>(normalized * static_cast<float>(options.sahBinCount)));
        return binIndex <= split.binSplit;
      });
  return static_cast<uint32_t>(midIt - primitives.begin());
}

static uint32_t partitionByCountMedian(std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const RBVHNodeStats &nodeStats)
{
  uint32_t splitAxis = 0u;
  float maxExtent = nodeStats.centroidBounds.maxPoint[0] - nodeStats.centroidBounds.minPoint[0];
  for (uint32_t axis = 1u; axis < 3u; ++axis)
  {
    const float extent = nodeStats.centroidBounds.maxPoint[axis] - nodeStats.centroidBounds.minPoint[axis];
    if (extent > maxExtent)
    {
      maxExtent = extent;
      splitAxis = axis;
    }
  }

  const uint32_t mid = begin + ((end - begin) >> 1u);
  std::nth_element(
      primitives.begin() + begin,
      primitives.begin() + mid,
      primitives.begin() + end,
      [splitAxis](const RBVHBuildPrimitive &a, const RBVHBuildPrimitive &b)
      {
        return a.centroid[splitAxis] < b.centroid[splitAxis];
      });
  return mid;
}

static uint32_t partitionBySpatialMedian(std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const RBVHSplit &split)
{
  auto midIt = std::partition(
      primitives.begin() + begin,
      primitives.begin() + end,
      [&split](const RBVHBuildPrimitive &primitive)
      {
        return primitive.centroid[split.axis] <= split.planePosition;
      });
  return static_cast<uint32_t>(midIt - primitives.begin());
}

static uint32_t partitionByNormalMedian(std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end)
{
  if (begin + 1u >= end)
  {
    return begin;
  }

  float minNormal[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  float maxNormal[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
  bool hasNormal = false;
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (!primitive.hasNormal)
    {
      continue;
    }
    hasNormal = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      minNormal[axis] = std::min(minNormal[axis], primitive.normal[axis]);
      maxNormal[axis] = std::max(maxNormal[axis], primitive.normal[axis]);
    }
  }
  if (!hasNormal)
  {
    return begin;
  }

  uint32_t splitAxis = 0u;
  float maxExtent = maxNormal[0] - minNormal[0];
  for (uint32_t axis = 1u; axis < 3u; ++axis)
  {
    const float extent = maxNormal[axis] - minNormal[axis];
    if (extent > maxExtent)
    {
      maxExtent = extent;
      splitAxis = axis;
    }
  }
  if (maxExtent <= 1.0e-6f)
  {
    return begin;
  }

  const uint32_t mid = begin + ((end - begin) >> 1u);
  std::nth_element(
      primitives.begin() + begin,
      primitives.begin() + mid,
      primitives.begin() + end,
      [splitAxis](const RBVHBuildPrimitive &a, const RBVHBuildPrimitive &b)
      {
        return a.normal[splitAxis] < b.normal[splitAxis];
      });
  return mid;
}

static uint32_t partitionBySplit(std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const RBVHSplit &split, const RBVHNodeStats &nodeStats, const RBVHBuildOptions &options)
{
  uint32_t mid = begin + ((end - begin) >> 1u);
  if (split.componentSplit)
  {
    auto midIt = std::partition(
        primitives.begin() + begin,
        primitives.begin() + end,
        [&split](const RBVHBuildPrimitive &primitive)
        {
          return std::find(split.leftComponentIds.begin(), split.leftComponentIds.end(), primitive.componentId) != split.leftComponentIds.end();
        });
    mid = static_cast<uint32_t>(midIt - primitives.begin());
  }
  else if (split.valid)
  {
    mid = split.spatialMedianSplit
        ? partitionBySpatialMedian(primitives, begin, end, split)
        : partitionBySahSplit(primitives, begin, end, split, nodeStats, options);
  }

  if (mid <= begin || mid >= end)
  {
    mid = partitionByCountMedian(primitives, begin, end, nodeStats);
  }
  return mid;
}

static void buildOverlappedSplitChildren(
    const std::vector<RBVHBuildPrimitive> &primitives,
    const uint32_t begin,
    const uint32_t end,
    const RBVHSplit &split,
    const RBVHNodeStats &nodeStats,
    const RBVHBuildOptions &options,
    const uint32_t mid,
    std::vector<RBVHBuildPrimitive> &left,
    std::vector<RBVHBuildPrimitive> &right)
{
  left.assign(primitives.begin() + begin, primitives.begin() + mid);
  right.assign(primitives.begin() + mid, primitives.begin() + end);

  if (split.componentSplit || !split.valid || end <= begin + 1u)
  {
    return;
  }

  const float nodeExtent = std::max(nodeStats.bounds.maxPoint[split.axis] - nodeStats.bounds.minPoint[split.axis], 0.0f);
  const float overlap = nodeExtent * std::max(options.splitOverlapFraction, 0.0f);
  if (!(overlap > 0.0f))
  {
    return;
  }

  const float leftDuplicateThreshold = split.planePosition + overlap;
  const float rightDuplicateThreshold = split.planePosition - overlap;

  struct OverlapCandidate
  {
    uint32_t index = 0u;
    bool duplicateToLeft = false;
    float distance = 0.0f;
  };
  std::vector<OverlapCandidate> duplicateCandidates;
  duplicateCandidates.reserve(end - begin);

  const auto overlapDistance = [&split](const RBVHBuildPrimitive &primitive)
  {
    return primitive.bounds.minPoint[split.axis] <= split.planePosition && primitive.bounds.maxPoint[split.axis] >= split.planePosition
        ? 0.0f
        : std::min(std::fabs(primitive.bounds.minPoint[split.axis] - split.planePosition), std::fabs(primitive.bounds.maxPoint[split.axis] - split.planePosition));
  };

  for (uint32_t index = begin; index < mid; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (primitive.bounds.maxPoint[split.axis] >= rightDuplicateThreshold && primitive.bounds.minPoint[split.axis] <= leftDuplicateThreshold)
    {
      duplicateCandidates.push_back(OverlapCandidate{.index = index, .duplicateToLeft = false, .distance = overlapDistance(primitive)});
    }
  }
  for (uint32_t index = mid; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (primitive.bounds.maxPoint[split.axis] >= rightDuplicateThreshold && primitive.bounds.minPoint[split.axis] <= leftDuplicateThreshold)
    {
      duplicateCandidates.push_back(OverlapCandidate{.index = index, .duplicateToLeft = true, .distance = overlapDistance(primitive)});
    }
  }

  std::sort(
      duplicateCandidates.begin(),
      duplicateCandidates.end(),
      [](const OverlapCandidate &a, const OverlapCandidate &b)
      {
        return a.distance < b.distance;
      });

  const uint32_t primitiveCount = end - begin;
  const float duplicateFraction = std::clamp(options.splitOverlapFraction, 0.0f, 0.5f);
  const uint32_t duplicateBudget = duplicateFraction > 0.0f
      ? static_cast<uint32_t>(std::floor(static_cast<float>(primitiveCount) * duplicateFraction))
      : 0u;
  for (uint32_t candidateIndex = 0u; candidateIndex < std::min<uint32_t>(duplicateBudget, static_cast<uint32_t>(duplicateCandidates.size())); ++candidateIndex)
  {
    const OverlapCandidate &candidate = duplicateCandidates[candidateIndex];
    const RBVHBuildPrimitive &primitive = primitives[candidate.index];

    if (candidate.duplicateToLeft)
    {
      left.push_back(primitive);
    }
    else
    {
      right.push_back(primitive);
    }
  }
}

static bool makeSpatialSplitDomains(
    const virtualgeometry::AABB &parentDomain,
    const RBVHSplit &split,
    const RBVHNodeStats &nodeStats,
    const RBVHBuildOptions &options,
    virtualgeometry::AABB &leftDomain,
    virtualgeometry::AABB &rightDomain)
{
  if (split.componentSplit || !split.valid)
  {
    return false;
  }

  const virtualgeometry::AABB domain = isValidAabb(parentDomain) ? parentDomain : nodeStats.bounds;
  if (!isValidAabb(domain))
  {
    return false;
  }

  const float nodeExtent = std::max(domain.maxPoint[split.axis] - domain.minPoint[split.axis], 0.0f);
  const float overlap = nodeExtent * std::max(options.splitOverlapFraction, 0.0f);
  const float leftMax = std::clamp(split.planePosition + overlap, domain.minPoint[split.axis], domain.maxPoint[split.axis]);
  const float rightMin = std::clamp(split.planePosition - overlap, domain.minPoint[split.axis], domain.maxPoint[split.axis]);
  if (!(leftMax > domain.minPoint[split.axis]) || !(rightMin < domain.maxPoint[split.axis]))
  {
    return false;
  }

  leftDomain = domain;
  rightDomain = domain;
  leftDomain.maxPoint[split.axis] = leftMax;
  rightDomain.minPoint[split.axis] = rightMin;
  return isValidAabb(leftDomain) && isValidAabb(rightDomain);
}

static bool buildClippedDomainPrimitives(
    const std::vector<RBVHBuildPrimitive> &sourcePrimitives,
    const uint32_t begin,
    const uint32_t end,
    const virtualgeometry::AABB &domain,
    std::vector<RBVHBuildPrimitive> &outPrimitives)
{
  if (!isValidAabb(domain))
  {
    return false;
  }

  outPrimitives.clear();
  outPrimitives.reserve(end - begin);
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = sourcePrimitives[index];
    if (!aabbIntersects(primitive.bounds, domain))
    {
      continue;
    }
    if (aabbContains(domain, primitive.bounds))
    {
      outPrimitives.push_back(primitive);
      continue;
    }

    RBVHBuildPrimitive clippedPrimitive{};
    if (clipPrimitiveToBounds(primitive, domain, clippedPrimitive))
    {
      outPrimitives.push_back(clippedPrimitive);
    }
  }
  return !outPrimitives.empty();
}

static std::vector<uint32_t> collectPrimitiveIndices(const std::vector<RBVHBuildPrimitive> &primitives, const uint32_t begin, const uint32_t end)
{
  std::vector<uint32_t> primitiveIndices;
  primitiveIndices.reserve(end - begin);
  for (uint32_t index = begin; index < end; ++index)
  {
    primitiveIndices.push_back(primitives[index].primitiveIndex);
  }
  return primitiveIndices;
}

static void emitCard(
    const std::vector<RBVHBuildPrimitive> &primitives,
    const uint32_t begin,
    const uint32_t end,
    const RBVHNodeStats &nodeStats,
    const virtualgeometry::AABB &clipBounds,
    const uint32_t sourceNodeIndex,
    const RBVHBuildOptions &options,
    RBVHBuildResult &result,
    std::vector<RBVHBuildPrimitive> &rasterPrimitiveRefs)
{
  std::vector<RBVHBuildPrimitive> cardPrimitives;
  cardPrimitives.reserve(end - begin);
  for (uint32_t index = begin; index < end; ++index)
  {
    const RBVHBuildPrimitive &primitive = primitives[index];
    if (!aabbIntersects(primitive.bounds, clipBounds))
    {
      continue;
    }
    if (aabbContains(clipBounds, primitive.bounds))
    {
      cardPrimitives.push_back(primitive);
      continue;
    }

    RBVHBuildPrimitive clippedPrimitive{};
    if (clipPrimitiveToBounds(primitive, clipBounds, clippedPrimitive))
    {
      cardPrimitives.push_back(clippedPrimitive);
    }
  }
  if (cardPrimitives.empty())
  {
    cardPrimitives.assign(primitives.begin() + begin, primitives.begin() + end);
  }

  std::vector<uint32_t> primitiveIndices = collectPrimitiveIndices(cardPrimitives, 0u, static_cast<uint32_t>(cardPrimitives.size()));
  const uint32_t primitiveOffset = static_cast<uint32_t>(result.sourcePrimitiveIndices.size());
  result.sourcePrimitiveIndices.insert(result.sourcePrimitiveIndices.end(), primitiveIndices.begin(), primitiveIndices.end());
  rasterPrimitiveRefs.insert(rasterPrimitiveRefs.end(), cardPrimitives.begin(), cardPrimitives.end());
  RBVHNodeStats cardStats = computeNodeStats(cardPrimitives, 0u, static_cast<uint32_t>(cardPrimitives.size()));
  cardStats.surface = selectSurfaceProjection(cardStats.surface.valid ? cardStats.surface : nodeStats.surface, options.refinementCriterion);
  result.cards.push_back(makeCard(clipBounds, sourceNodeIndex, cardPrimitives, 0u, static_cast<uint32_t>(cardPrimitives.size()), primitiveOffset, options, cardStats.surface));
}

static bool shouldEmitCard(const RBVHNodeStats &nodeStats, const uint32_t primitiveCount, const RBVHBuildOptions &options)
{
  if (primitiveCount <= 1u)
  {
    return true;
  }

  const SurfaceStats &stats = nodeStats.surface;
  if (!stats.valid)
  {
    return false;
  }

  bool refinementCriterionSatisfied = false;
  if (options.refinementCriterion == RBVHRefinementCriterion::ProjectedArea)
  {
    refinementCriterionSatisfied = stats.projectedAreaRatio >= options.projectedAreaThreshold;
  }
  else
  {
    refinementCriterionSatisfied = stats.minNormalDot >= options.minNormalDot;
  }
  if (!refinementCriterionSatisfied)
  {
    return false;
  }

  if (options.targetTileResolution > 0.0f && estimatePaperTileResolution(stats, options) > options.targetTileResolution)
  {
    return false;
  }
  return true;
}

struct RBVHConstructionRef
{
  uint32_t primitiveIndex = 0u;
  uint32_t nodeRef = 1u;
};

static std::vector<RBVHBuildPrimitive> gatherConstructionPrimitives(
    const std::vector<RBVHBuildPrimitive> &primitives,
    const std::vector<RBVHConstructionRef> &refs,
    const uint32_t begin,
    const uint32_t end)
{
  std::vector<RBVHBuildPrimitive> gathered;
  gathered.reserve(end - begin);
  for (uint32_t index = begin; index < end; ++index)
  {
    if (refs[index].primitiveIndex < primitives.size())
    {
      gathered.push_back(primitives[refs[index].primitiveIndex]);
    }
  }
  return gathered;
}

static uint32_t classifySpatialMedianChildren(
    const RBVHBuildPrimitive &primitive,
    const uint32_t primitiveOrdinal,
    const RBVHSplit &split,
    const RBVHNodeStats &nodeStats,
    const RBVHBuildOptions &options)
{
  if (!split.valid || split.componentSplit)
  {
    return 0u;
  }

  const float nodeExtent = std::max(nodeStats.bounds.maxPoint[split.axis] - nodeStats.bounds.minPoint[split.axis], 0.0f);
  const float overlap = nodeExtent * std::max(options.splitOverlapFraction, 0.0f);
  if (!(overlap > 0.0f))
  {
    if (primitive.centroid[split.axis] < split.planePosition)
    {
      return 1u;
    }
    if (primitive.centroid[split.axis] > split.planePosition)
    {
      return 2u;
    }
    return (primitiveOrdinal & 1u) == 0u ? 1u : 2u;
  }

  const float leftMax = split.planePosition + overlap;
  const float rightMin = split.planePosition - overlap;

  uint32_t mask = 0u;
  const bool overlapsSafetyBand =
      primitive.bounds.minPoint[split.axis] <= leftMax &&
      primitive.bounds.maxPoint[split.axis] >= rightMin;
  if (overlapsSafetyBand)
  {
    return 3u;
  }

  if (primitive.centroid[split.axis] <= split.planePosition)
  {
    mask |= 1u;
  }
  else
  {
    mask |= 2u;
  }
  if (mask == 0u)
  {
    const float center = primitive.centroid[split.axis];
    mask = center <= split.planePosition ? 1u : 2u;
  }
  return mask;
}

static bool appendNormalMedianChildren(
    const std::vector<RBVHConstructionRef> &activeRefs,
    const std::vector<RBVHBuildPrimitive> &nodePrimitives,
    const uint32_t begin,
    const uint32_t end,
    const uint32_t nodeRef,
    std::vector<RBVHConstructionRef> &nextRefs)
{
  if (nodePrimitives.size() < 2u)
  {
    return false;
  }

  float minNormal[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
  float maxNormal[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
  bool hasNormal = false;
  for (const RBVHBuildPrimitive &primitive : nodePrimitives)
  {
    if (!primitive.hasNormal)
    {
      continue;
    }
    hasNormal = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      minNormal[axis] = std::min(minNormal[axis], primitive.normal[axis]);
      maxNormal[axis] = std::max(maxNormal[axis], primitive.normal[axis]);
    }
  }
  if (!hasNormal)
  {
    return false;
  }

  uint32_t splitAxis = 0u;
  float maxExtent = maxNormal[0] - minNormal[0];
  for (uint32_t axis = 1u; axis < 3u; ++axis)
  {
    const float extent = maxNormal[axis] - minNormal[axis];
    if (extent > maxExtent)
    {
      maxExtent = extent;
      splitAxis = axis;
    }
  }
  if (maxExtent <= 1.0e-6f)
  {
    return false;
  }

  std::vector<uint32_t> order(nodePrimitives.size());
  for (uint32_t index = 0u; index < order.size(); ++index)
  {
    order[index] = index;
  }
  const uint32_t mid = static_cast<uint32_t>(order.size() / 2u);
  std::nth_element(
      order.begin(),
      order.begin() + mid,
      order.end(),
      [&nodePrimitives, splitAxis](const uint32_t a, const uint32_t b)
      {
        return nodePrimitives[a].normal[splitAxis] < nodePrimitives[b].normal[splitAxis];
      });

  std::vector<uint8_t> goesLeft(order.size(), 0u);
  for (uint32_t index = 0u; index < mid; ++index)
  {
    goesLeft[order[index]] = 1u;
  }

  if (mid == 0u || mid >= order.size())
  {
    return false;
  }

  for (uint32_t index = begin; index < end; ++index)
  {
    const uint32_t localIndex = index - begin;
    nextRefs.push_back(
        RBVHConstructionRef{
          .primitiveIndex = activeRefs[index].primitiveIndex,
          .nodeRef = goesLeft[localIndex] != 0u ? nodeRef * 2u : nodeRef * 2u + 1u,
        });
  }
  return true;
}

static void appendFinalRefs(
    const std::vector<RBVHConstructionRef> &refs,
    const uint32_t begin,
    const uint32_t end,
    std::vector<RBVHConstructionRef> &finalRefs)
{
  finalRefs.insert(finalRefs.end(), refs.begin() + begin, refs.begin() + end);
}

static void emitFinalLeafCards(
    const std::vector<RBVHBuildPrimitive> &primitives,
    std::vector<RBVHConstructionRef> &finalRefs,
    const RBVHBuildOptions &options,
    RBVHBuildResult &result,
    std::vector<RBVHBuildPrimitive> &rasterPrimitiveRefs)
{
  std::sort(
      finalRefs.begin(),
      finalRefs.end(),
      [](const RBVHConstructionRef &a, const RBVHConstructionRef &b)
      {
        if (a.nodeRef != b.nodeRef)
        {
          return a.nodeRef < b.nodeRef;
        }
        return a.primitiveIndex < b.primitiveIndex;
      });

  for (uint32_t begin = 0u; begin < finalRefs.size();)
  {
    const uint32_t nodeRef = finalRefs[begin].nodeRef;
    uint32_t end = begin + 1u;
    while (end < finalRefs.size() && finalRefs[end].nodeRef == nodeRef)
    {
      ++end;
    }

    std::vector<RBVHBuildPrimitive> leafPrimitives = gatherConstructionPrimitives(primitives, finalRefs, begin, end);
    if (!leafPrimitives.empty())
    {
      RBVHNodeStats leafStats = computeNodeStats(leafPrimitives, 0u, static_cast<uint32_t>(leafPrimitives.size()));
      RBVHBuildOptions leafOptions = adaptiveOptionsForNode(options, leafStats);
      leafStats.surface = selectSurfaceProjection(leafStats.surface, leafOptions.refinementCriterion);
      emitCard(
          leafPrimitives,
          0u,
          static_cast<uint32_t>(leafPrimitives.size()),
          leafStats,
          leafStats.bounds,
          nodeRef,
          leafOptions,
          result,
          rasterPrimitiveRefs);
    }
    begin = end;
  }
}

static void buildCardsBreadthFirst(
    const std::vector<RBVHBuildPrimitive> &primitives,
    const RBVHBuildOptions &options,
    RBVHBuildResult &result,
    std::vector<RBVHBuildPrimitive> &rasterPrimitiveRefs)
{
  std::vector<RBVHConstructionRef> activeRefs;
  activeRefs.reserve(primitives.size());
  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitives.size(); ++primitiveIndex)
  {
    activeRefs.push_back(RBVHConstructionRef{.primitiveIndex = primitiveIndex, .nodeRef = 1u});
  }

  std::vector<RBVHConstructionRef> finalRefs;
  finalRefs.reserve(primitives.size());

  const uint32_t maxCards = std::max(options.maxCards, 1u);
  uint32_t finalNodeCount = 0u;
  auto appendFinalNode = [&](const uint32_t begin, const uint32_t end)
  {
    appendFinalRefs(activeRefs, begin, end, finalRefs);
    ++finalNodeCount;
  };

  uint32_t depth = 0u;
  while (!activeRefs.empty())
  {
    std::sort(
        activeRefs.begin(),
        activeRefs.end(),
        [](const RBVHConstructionRef &a, const RBVHConstructionRef &b)
        {
          if (a.nodeRef != b.nodeRef)
          {
            return a.nodeRef < b.nodeRef;
          }
          return a.primitiveIndex < b.primitiveIndex;
        });

    std::vector<RBVHConstructionRef> nextRefs;
    nextRefs.reserve(activeRefs.size() * 2u);

    uint32_t activeNodeCount = 0u;
    for (uint32_t begin = 0u; begin < activeRefs.size();)
    {
      const uint32_t nodeRef = activeRefs[begin].nodeRef;
      uint32_t end = begin + 1u;
      while (end < activeRefs.size() && activeRefs[end].nodeRef == nodeRef)
      {
        ++end;
      }
      ++activeNodeCount;
      begin = end;
    }

    const uint32_t splitBudget =
        maxCards > finalNodeCount + activeNodeCount ? maxCards - finalNodeCount - activeNodeCount : 0u;
    uint32_t splitNodeCount = 0u;

    for (uint32_t begin = 0u; begin < activeRefs.size();)
    {
      const uint32_t nodeRef = activeRefs[begin].nodeRef;
      uint32_t end = begin + 1u;
      while (end < activeRefs.size() && activeRefs[end].nodeRef == nodeRef)
      {
        ++end;
      }

      std::vector<RBVHBuildPrimitive> nodePrimitives = gatherConstructionPrimitives(primitives, activeRefs, begin, end);
      RBVHNodeStats nodeStats = computeNodeStats(nodePrimitives, 0u, static_cast<uint32_t>(nodePrimitives.size()));
      RBVHBuildOptions nodeOptions = adaptiveOptionsForNode(options, nodeStats);
      nodeStats.surface = selectSurfaceProjection(nodeStats.surface, nodeOptions.refinementCriterion);

      const bool canCreateChildren = nodeRef <= (std::numeric_limits<uint32_t>::max() - 1u) / 2u && depth < 30u;
      const bool canSpendCardBudgetOnSplit = splitNodeCount < splitBudget;
      const bool rasterize =
          !canCreateChildren ||
          !canSpendCardBudgetOnSplit ||
          shouldEmitCard(nodeStats, static_cast<uint32_t>(nodePrimitives.size()), nodeOptions);
      if (rasterize)
      {
        appendFinalNode(begin, end);
        begin = end;
        continue;
      }

      RBVHSplit split = findSpatialMedianSplit(0u, static_cast<uint32_t>(nodePrimitives.size()), nodeStats);
      if (!split.valid)
      {
        appendFinalNode(begin, end);
        begin = end;
        continue;
      }

      uint32_t leftCount = 0u;
      uint32_t rightCount = 0u;
      for (uint32_t localIndex = 0u; localIndex < nodePrimitives.size(); ++localIndex)
      {
        const uint32_t childMask = classifySpatialMedianChildren(nodePrimitives[localIndex], localIndex, split, nodeStats, nodeOptions);
        leftCount += (childMask & 1u) != 0u ? 1u : 0u;
        rightCount += (childMask & 2u) != 0u ? 1u : 0u;
      }

      const uint32_t primitiveCount = static_cast<uint32_t>(nodePrimitives.size());
      if (leftCount == 0u || rightCount == 0u || (leftCount >= primitiveCount && rightCount >= primitiveCount))
      {
        if (!appendNormalMedianChildren(activeRefs, nodePrimitives, begin, end, nodeRef, nextRefs))
        {
          appendFinalNode(begin, end);
        }
        else
        {
          ++splitNodeCount;
        }
        begin = end;
        continue;
      }

      for (uint32_t index = begin; index < end; ++index)
      {
        const RBVHBuildPrimitive &primitive = primitives[activeRefs[index].primitiveIndex];
        const uint32_t childMask = classifySpatialMedianChildren(primitive, index - begin, split, nodeStats, nodeOptions);
        if ((childMask & 1u) != 0u)
        {
          nextRefs.push_back(RBVHConstructionRef{.primitiveIndex = activeRefs[index].primitiveIndex, .nodeRef = nodeRef * 2u});
        }
        if ((childMask & 2u) != 0u)
        {
          nextRefs.push_back(RBVHConstructionRef{.primitiveIndex = activeRefs[index].primitiveIndex, .nodeRef = nodeRef * 2u + 1u});
        }
      }
      ++splitNodeCount;

      begin = end;
    }

    activeRefs = std::move(nextRefs);
    ++depth;
  }

  emitFinalLeafCards(primitives, finalRefs, options, result, rasterPrimitiveRefs);
}

static void buildCardsRecursive(
    std::vector<RBVHBuildPrimitive> &primitives,
    const uint32_t begin,
    const uint32_t end,
    const RBVHBuildOptions &options,
    const virtualgeometry::AABB &clipBounds,
    uint32_t &nextNodeIndex,
    const uint32_t depth,
    RBVHBuildResult &result,
    std::vector<RBVHBuildPrimitive> &rasterPrimitiveRefs)
{
  if (begin >= end)
  {
    return;
  }

  const uint32_t sourceNodeIndex = nextNodeIndex++;
  RBVHNodeStats nodeStats = computeNodeStats(primitives, begin, end);
  const RBVHBuildOptions nodeOptions = adaptiveOptionsForNode(options, nodeStats);
  nodeStats.surface = selectSurfaceProjection(nodeStats.surface, nodeOptions.refinementCriterion);
  RBVHSplit split = nodeOptions.splitStrategy == RBVHSplitStrategy::SpatialMedian
      ? findSpatialMedianSplit(begin, end, nodeStats)
      : findSahSplit(primitives, begin, end, nodeStats, nodeOptions);

  const uint32_t primitiveCount = end - begin;
  if (depth >= 64u)
  {
    emitCard(primitives, begin, end, nodeStats, clipBounds, sourceNodeIndex, nodeOptions, result, rasterPrimitiveRefs);
    return;
  }

  if (shouldEmitCard(nodeStats, primitiveCount, nodeOptions))
  {
    emitCard(primitives, begin, end, nodeStats, clipBounds, sourceNodeIndex, nodeOptions, result, rasterPrimitiveRefs);
    return;
  }

  if (!split.valid)
  {
    const uint32_t mid = partitionByNormalMedian(primitives, begin, end);
    if (mid <= begin || mid >= end)
    {
      emitCard(primitives, begin, end, nodeStats, clipBounds, sourceNodeIndex, nodeOptions, result, rasterPrimitiveRefs);
      return;
    }

    std::vector<RBVHBuildPrimitive> leftPrimitives(primitives.begin() + begin, primitives.begin() + mid);
    std::vector<RBVHBuildPrimitive> rightPrimitives(primitives.begin() + mid, primitives.begin() + end);
    const RBVHNodeStats leftStats = computeNodeStats(leftPrimitives, 0u, static_cast<uint32_t>(leftPrimitives.size()));
    const RBVHNodeStats rightStats = computeNodeStats(rightPrimitives, 0u, static_cast<uint32_t>(rightPrimitives.size()));
    buildCardsRecursive(leftPrimitives, 0u, static_cast<uint32_t>(leftPrimitives.size()), options, leftStats.bounds, nextNodeIndex, depth + 1u, result, rasterPrimitiveRefs);
    buildCardsRecursive(rightPrimitives, 0u, static_cast<uint32_t>(rightPrimitives.size()), options, rightStats.bounds, nextNodeIndex, depth + 1u, result, rasterPrimitiveRefs);
    return;
  }

  virtualgeometry::AABB leftDomain{};
  virtualgeometry::AABB rightDomain{};
  if (makeSpatialSplitDomains(clipBounds, split, nodeStats, nodeOptions, leftDomain, rightDomain))
  {
    std::vector<RBVHBuildPrimitive> leftPrimitives;
    std::vector<RBVHBuildPrimitive> rightPrimitives;
    if (buildClippedDomainPrimitives(primitives, begin, end, leftDomain, leftPrimitives) &&
        buildClippedDomainPrimitives(primitives, begin, end, rightDomain, rightPrimitives))
    {
      const RBVHNodeStats leftStats = computeNodeStats(leftPrimitives, 0u, static_cast<uint32_t>(leftPrimitives.size()));
      const RBVHNodeStats rightStats = computeNodeStats(rightPrimitives, 0u, static_cast<uint32_t>(rightPrimitives.size()));
      if (leftStats.surface.surfaceArea < nodeStats.surface.surfaceArea * 0.999f &&
          rightStats.surface.surfaceArea < nodeStats.surface.surfaceArea * 0.999f)
      {
        buildCardsRecursive(leftPrimitives, 0u, static_cast<uint32_t>(leftPrimitives.size()), options, leftDomain, nextNodeIndex, depth + 1u, result, rasterPrimitiveRefs);
        buildCardsRecursive(rightPrimitives, 0u, static_cast<uint32_t>(rightPrimitives.size()), options, rightDomain, nextNodeIndex, depth + 1u, result, rasterPrimitiveRefs);
        return;
      }
    }
  }

  emitCard(primitives, begin, end, nodeStats, clipBounds, sourceNodeIndex, nodeOptions, result, rasterPrimitiveRefs);
}

static uint16_t quantizeDepth16(const float depth01)
{
  const float clamped = std::clamp(depth01, 0.0f, 1.0f);
  return static_cast<uint16_t>(std::round(clamped * 65535.0f));
}

static uint32_t packDepthMinMax16(const uint16_t minDepth, const uint16_t maxDepth)
{
  return static_cast<uint32_t>(minDepth) | (static_cast<uint32_t>(maxDepth) << 16u);
}

static bool depthMinMaxIsValid(const uint32_t packed)
{
  return (packed & 0xffffu) <= (packed >> 16u);
}

static uint32_t mergeDepthMinMax16(const uint32_t a, const uint32_t b)
{
  const bool validA = depthMinMaxIsValid(a);
  const bool validB = depthMinMaxIsValid(b);
  if (!validA)
  {
    return validB ? b : packDepthMinMax16(0xffffu, 0u);
  }
  if (!validB)
  {
    return a;
  }

  const uint16_t minDepth = static_cast<uint16_t>(std::min(a & 0xffffu, b & 0xffffu));
  const uint16_t maxDepth = static_cast<uint16_t>(std::max(a >> 16u, b >> 16u));
  return packDepthMinMax16(minDepth, maxDepth);
}

static void rasterizeTriangleToAtlas(
    const RBVHCardPrimitive &card,
    const uint32_t atlasSize,
    const float positions[3][3],
    std::vector<uint16_t> &depthAtlas,
    std::vector<uint8_t> &coverageAtlas)
{
  const uint32_t tileWidth = card.atlasWidth;
  const uint32_t tileHeight = card.atlasHeight;
  if (tileWidth == 0u || tileHeight == 0u || atlasSize == 0u)
  {
    return;
  }

  const float depthRange = std::max(card.depthMax - card.depthMin, 1.0e-8f);
  float uv[3][2]{};
  float depth[3]{};
  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    float local[3]{};
    sub3(positions[corner], card.origin, local);
    uv[corner][0] = dot3(local, card.axisU);
    uv[corner][1] = dot3(local, card.axisV);
    depth[corner] = (dot3(positions[corner], card.axisN) - card.depthMin) / depthRange;
  }

  const float denom = edge2(uv[0][0], uv[0][1], uv[1][0], uv[1][1], uv[2][0], uv[2][1]);
  if (std::fabs(denom) <= 1.0e-12f)
  {
    return;
  }

  const float minU = std::max(-card.halfExtentU, std::min({uv[0][0], uv[1][0], uv[2][0]}));
  const float maxU = std::min(card.halfExtentU, std::max({uv[0][0], uv[1][0], uv[2][0]}));
  const float minV = std::max(-card.halfExtentV, std::min({uv[0][1], uv[1][1], uv[2][1]}));
  const float maxV = std::min(card.halfExtentV, std::max({uv[0][1], uv[1][1], uv[2][1]}));

  const int minX = std::clamp(static_cast<int>(std::floor((minU / card.halfExtentU * 0.5f + 0.5f) * static_cast<float>(tileWidth))), 0, static_cast<int>(tileWidth) - 1);
  const int maxX = std::clamp(static_cast<int>(std::ceil((maxU / card.halfExtentU * 0.5f + 0.5f) * static_cast<float>(tileWidth))), 0, static_cast<int>(tileWidth) - 1);
  const int minY = std::clamp(static_cast<int>(std::floor((minV / card.halfExtentV * 0.5f + 0.5f) * static_cast<float>(tileHeight))), 0, static_cast<int>(tileHeight) - 1);
  const int maxY = std::clamp(static_cast<int>(std::ceil((maxV / card.halfExtentV * 0.5f + 0.5f) * static_cast<float>(tileHeight))), 0, static_cast<int>(tileHeight) - 1);

  for (int y = minY; y <= maxY; ++y)
  {
    const float pixelMinV = (static_cast<float>(y) / static_cast<float>(tileHeight) * 2.0f - 1.0f) * card.halfExtentV;
    const float pixelMaxV = ((static_cast<float>(y) + 1.0f) / static_cast<float>(tileHeight) * 2.0f - 1.0f) * card.halfExtentV;
    const float sampleV = ((static_cast<float>(y) + 0.5f) / static_cast<float>(tileHeight) * 2.0f - 1.0f) * card.halfExtentV;
    for (int x = minX; x <= maxX; ++x)
    {
      const float pixelMinU = (static_cast<float>(x) / static_cast<float>(tileWidth) * 2.0f - 1.0f) * card.halfExtentU;
      const float pixelMaxU = ((static_cast<float>(x) + 1.0f) / static_cast<float>(tileWidth) * 2.0f - 1.0f) * card.halfExtentU;
      const float sampleU = ((static_cast<float>(x) + 0.5f) / static_cast<float>(tileWidth) * 2.0f - 1.0f) * card.halfExtentU;

      float depth01 = 0.0f;
      float bestDistance2 = std::numeric_limits<float>::max();
      bool hasSample = false;
      const auto trySample = [&](const float u, const float v)
      {
        constexpr float kSampleEpsilon = 1.0e-5f;
        if (u < pixelMinU - kSampleEpsilon || u > pixelMaxU + kSampleEpsilon ||
            v < pixelMinV - kSampleEpsilon || v > pixelMaxV + kSampleEpsilon)
        {
          return;
        }

        const float w0 = edge2(uv[1][0], uv[1][1], uv[2][0], uv[2][1], u, v) / denom;
        const float w1 = edge2(uv[2][0], uv[2][1], uv[0][0], uv[0][1], u, v) / denom;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -kSampleEpsilon || w1 < -kSampleEpsilon || w2 < -kSampleEpsilon)
        {
          return;
        }

        const float du = u - sampleU;
        const float dv = v - sampleV;
        const float distance2 = du * du + dv * dv;
        if (!hasSample || distance2 < bestDistance2)
        {
          hasSample = true;
          bestDistance2 = distance2;
          depth01 = w0 * depth[0] + w1 * depth[1] + w2 * depth[2];
        }
      };

      trySample(sampleU, sampleV);
      trySample(pixelMinU, pixelMinV);
      trySample(pixelMaxU, pixelMinV);
      trySample(pixelMinU, pixelMaxV);
      trySample(pixelMaxU, pixelMaxV);

      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        trySample(uv[corner][0], uv[corner][1]);
      }

      for (uint32_t edge = 0u; edge < 3u; ++edge)
      {
        const uint32_t next = (edge + 1u) % 3u;
        const float u0 = uv[edge][0];
        const float v0 = uv[edge][1];
        const float u1 = uv[next][0];
        const float v1 = uv[next][1];
        const float du = u1 - u0;
        const float dv = v1 - v0;
        if (std::fabs(du) > 1.0e-8f)
        {
          const float tMin = (pixelMinU - u0) / du;
          if (tMin >= 0.0f && tMin <= 1.0f)
            trySample(pixelMinU, v0 + dv * tMin);
          const float tMax = (pixelMaxU - u0) / du;
          if (tMax >= 0.0f && tMax <= 1.0f)
            trySample(pixelMaxU, v0 + dv * tMax);
        }
        if (std::fabs(dv) > 1.0e-8f)
        {
          const float tMin = (pixelMinV - v0) / dv;
          if (tMin >= 0.0f && tMin <= 1.0f)
            trySample(u0 + du * tMin, pixelMinV);
          const float tMax = (pixelMaxV - v0) / dv;
          if (tMax >= 0.0f && tMax <= 1.0f)
            trySample(u0 + du * tMax, pixelMaxV);
        }
      }

      if (!hasSample)
      {
        continue;
      }

      const uint32_t pixelIndex = (card.atlasY + static_cast<uint32_t>(y)) * atlasSize + card.atlasX + static_cast<uint32_t>(x);
      if (pixelIndex >= depthAtlas.size() || pixelIndex >= coverageAtlas.size())
      {
        continue;
      }

      const uint16_t encodedDepth = quantizeDepth16(depth01);
      if (coverageAtlas[pixelIndex] == 0u || encodedDepth < depthAtlas[pixelIndex])
      {
        coverageAtlas[pixelIndex] = 1u;
        depthAtlas[pixelIndex] = encodedDepth;
      }
    }
  }
}

static void rasterizeCardToAtlas(
    const RBVHCardPrimitive &card,
    const uint32_t atlasSize,
    const std::vector<RBVHBuildPrimitive> &sourcePrimitiveRefs,
    std::vector<uint16_t> &depthAtlas,
    std::vector<uint8_t> &coverageAtlas)
{
  if (card.sourcePrimitiveOffset + card.sourcePrimitiveCount > sourcePrimitiveRefs.size())
  {
    return;
  }

  for (uint32_t sourceIndex = 0u; sourceIndex < card.sourcePrimitiveCount; ++sourceIndex)
  {
    const RBVHBuildPrimitive &primitive = sourcePrimitiveRefs[card.sourcePrimitiveOffset + sourceIndex];
    if (primitive.vertexCount < 3u)
    {
      continue;
    }

    for (uint32_t vertexIndex = 1u; vertexIndex + 1u < primitive.vertexCount; ++vertexIndex)
    {
      float positions[3][3]{};
      copy3(primitive.vertices[0u], positions[0u]);
      copy3(primitive.vertices[vertexIndex], positions[1u]);
      copy3(primitive.vertices[vertexIndex + 1u], positions[2u]);
      rasterizeTriangleToAtlas(card, atlasSize, positions, depthAtlas, coverageAtlas);
    }
  }
}

static bool readTileDepthSample(
    const std::vector<uint16_t> &tileDepth,
    const std::vector<uint8_t> &tileCoverage,
    const uint32_t tileWidth,
    const uint32_t tileHeight,
    const int x,
    const int y,
    float &outDepth)
{
  if (x < 0 || y < 0 || x >= static_cast<int>(tileWidth) || y >= static_cast<int>(tileHeight))
  {
    return false;
  }

  const uint32_t pixelIndex = static_cast<uint32_t>(y) * tileWidth + static_cast<uint32_t>(x);
  if (pixelIndex >= tileDepth.size() || pixelIndex >= tileCoverage.size() || tileCoverage[pixelIndex] == 0u)
  {
    return false;
  }

  outDepth = static_cast<float>(tileDepth[pixelIndex]);
  return true;
}

static uint16_t extrapolateTileDepth(
    const std::vector<uint16_t> &tileDepth,
    const std::vector<uint8_t> &tileCoverage,
    const uint32_t tileWidth,
    const uint32_t tileHeight,
    const int sourceX,
    const int sourceY,
    const int targetX,
    const int targetY)
{
  float centerDepth = 0.0f;
  if (!readTileDepthSample(tileDepth, tileCoverage, tileWidth, tileHeight, sourceX, sourceY, centerDepth))
  {
    return 0xffffu;
  }

  float leftDepth = 0.0f;
  float rightDepth = 0.0f;
  float upDepth = 0.0f;
  float downDepth = 0.0f;
  const bool hasLeft = readTileDepthSample(tileDepth, tileCoverage, tileWidth, tileHeight, sourceX - 1, sourceY, leftDepth);
  const bool hasRight = readTileDepthSample(tileDepth, tileCoverage, tileWidth, tileHeight, sourceX + 1, sourceY, rightDepth);
  const bool hasUp = readTileDepthSample(tileDepth, tileCoverage, tileWidth, tileHeight, sourceX, sourceY - 1, upDepth);
  const bool hasDown = readTileDepthSample(tileDepth, tileCoverage, tileWidth, tileHeight, sourceX, sourceY + 1, downDepth);

  float gradientX = 0.0f;
  if (hasLeft && hasRight)
  {
    gradientX = (rightDepth - leftDepth) * 0.5f;
  }
  else if (hasRight)
  {
    gradientX = rightDepth - centerDepth;
  }
  else if (hasLeft)
  {
    gradientX = centerDepth - leftDepth;
  }

  float gradientY = 0.0f;
  if (hasUp && hasDown)
  {
    gradientY = (downDepth - upDepth) * 0.5f;
  }
  else if (hasDown)
  {
    gradientY = downDepth - centerDepth;
  }
  else if (hasUp)
  {
    gradientY = centerDepth - upDepth;
  }

  const float extrapolatedDepth = centerDepth +
      gradientX * static_cast<float>(targetX - sourceX) +
      gradientY * static_cast<float>(targetY - sourceY);
  return static_cast<uint16_t>(std::round(std::clamp(extrapolatedDepth, 0.0f, 65535.0f)));
}

static void dilateCardTile(
    const RBVHCardPrimitive &card,
    const uint32_t atlasSize,
    std::vector<uint16_t> &depthAtlas,
    std::vector<uint8_t> &coverageAtlas,
    const uint32_t iterationCount)
{
  if (card.atlasWidth == 0u || card.atlasHeight == 0u || atlasSize == 0u || iterationCount == 0u)
  {
    return;
  }

  const size_t tilePixelCount = static_cast<size_t>(card.atlasWidth) * static_cast<size_t>(card.atlasHeight);
  std::vector<uint16_t> tileDepth(tilePixelCount, 0xffffu);
  std::vector<uint8_t> tileCoverage(tilePixelCount, 0u);

  for (uint32_t localY = 0u; localY < card.atlasHeight; ++localY)
  {
    for (uint32_t localX = 0u; localX < card.atlasWidth; ++localX)
    {
      const uint32_t atlasX = card.atlasX + localX;
      const uint32_t atlasY = card.atlasY + localY;
      const uint32_t atlasPixelIndex = atlasY * atlasSize + atlasX;
      const uint32_t tilePixelIndex = localY * card.atlasWidth + localX;
      if (atlasPixelIndex >= depthAtlas.size() || atlasPixelIndex >= coverageAtlas.size())
      {
        continue;
      }

      tileDepth[tilePixelIndex] = depthAtlas[atlasPixelIndex];
      tileCoverage[tilePixelIndex] = coverageAtlas[atlasPixelIndex];
    }
  }

  for (uint32_t iteration = 0u; iteration < iterationCount; ++iteration)
  {
    std::vector<uint16_t> nextDepth = tileDepth;
    std::vector<uint8_t> nextCoverage = tileCoverage;
    bool changed = false;

    for (uint32_t localY = 0u; localY < card.atlasHeight; ++localY)
    {
      for (uint32_t localX = 0u; localX < card.atlasWidth; ++localX)
      {
        const uint32_t pixelIndex = localY * card.atlasWidth + localX;
        if (tileCoverage[pixelIndex] != 0u)
        {
          continue;
        }

        uint32_t copyPixelIndex = UINT32_MAX;
        int copyLocalX = 0;
        int copyLocalY = 0;
        for (int dy = -1; dy <= 1 && copyPixelIndex == UINT32_MAX; ++dy)
        {
          const int neighborLocalY = static_cast<int>(localY) + dy;
          if (neighborLocalY < 0 || neighborLocalY >= static_cast<int>(card.atlasHeight))
          {
            continue;
          }

          for (int dx = -1; dx <= 1; ++dx)
          {
            if (dx == 0 && dy == 0)
            {
              continue;
            }
            const int neighborLocalX = static_cast<int>(localX) + dx;
            if (neighborLocalX < 0 || neighborLocalX >= static_cast<int>(card.atlasWidth))
            {
              continue;
            }

            const uint32_t neighborPixelIndex = static_cast<uint32_t>(neighborLocalY) * card.atlasWidth + static_cast<uint32_t>(neighborLocalX);
            if (tileCoverage[neighborPixelIndex] != 0u)
            {
              copyPixelIndex = neighborPixelIndex;
              copyLocalX = neighborLocalX;
              copyLocalY = neighborLocalY;
              break;
            }
          }
        }

        if (copyPixelIndex == UINT32_MAX || copyPixelIndex >= tileDepth.size())
        {
          continue;
        }

        nextCoverage[pixelIndex] = 2u;
        nextDepth[pixelIndex] = extrapolateTileDepth(
            tileDepth,
            tileCoverage,
            card.atlasWidth,
            card.atlasHeight,
            copyLocalX,
            copyLocalY,
            static_cast<int>(localX),
            static_cast<int>(localY));
        changed = true;
      }
    }

    if (!changed)
    {
      break;
    }

    tileDepth = std::move(nextDepth);
    tileCoverage = std::move(nextCoverage);
  }

  for (uint32_t localY = 0u; localY < card.atlasHeight; ++localY)
  {
    for (uint32_t localX = 0u; localX < card.atlasWidth; ++localX)
    {
      const uint32_t atlasX = card.atlasX + localX;
      const uint32_t atlasY = card.atlasY + localY;
      const uint32_t atlasPixelIndex = atlasY * atlasSize + atlasX;
      const uint32_t tilePixelIndex = localY * card.atlasWidth + localX;
      if (atlasPixelIndex >= depthAtlas.size() || atlasPixelIndex >= coverageAtlas.size())
      {
        continue;
      }

      depthAtlas[atlasPixelIndex] = tileDepth[tilePixelIndex];
      coverageAtlas[atlasPixelIndex] = tileCoverage[tilePixelIndex];
    }
  }
}

static void prefilterCardTileDepth(
    const RBVHCardPrimitive &card,
    const uint32_t atlasSize,
    std::vector<uint16_t> &depthAtlas,
    const std::vector<uint8_t> &coverageAtlas)
{
  if (card.atlasWidth == 0u || card.atlasHeight == 0u || atlasSize == 0u)
  {
    return;
  }

  const size_t tilePixelCount = static_cast<size_t>(card.atlasWidth) * static_cast<size_t>(card.atlasHeight);
  std::vector<uint16_t> tileDepth(tilePixelCount, 0xffffu);
  std::vector<uint8_t> tileCoverage(tilePixelCount, 0u);

  for (uint32_t localY = 0u; localY < card.atlasHeight; ++localY)
  {
    for (uint32_t localX = 0u; localX < card.atlasWidth; ++localX)
    {
      const uint32_t atlasPixelIndex = (card.atlasY + localY) * atlasSize + card.atlasX + localX;
      const uint32_t tilePixelIndex = localY * card.atlasWidth + localX;
      if (atlasPixelIndex >= depthAtlas.size() || atlasPixelIndex >= coverageAtlas.size())
      {
        continue;
      }

      tileDepth[tilePixelIndex] = depthAtlas[atlasPixelIndex];
      tileCoverage[tilePixelIndex] = coverageAtlas[atlasPixelIndex];
    }
  }

  std::vector<uint16_t> filteredDepth = tileDepth;
  for (uint32_t localY = 0u; localY < card.atlasHeight; ++localY)
  {
    for (uint32_t localX = 0u; localX < card.atlasWidth; ++localX)
    {
      const uint32_t pixelIndex = localY * card.atlasWidth + localX;
      if (tileCoverage[pixelIndex] == 0u)
      {
        continue;
      }

      float weightSum = 0.0f;
      float depthSum = 0.0f;
      for (int dy = -1; dy <= 1; ++dy)
      {
        const int neighborY = static_cast<int>(localY) + dy;
        if (neighborY < 0 || neighborY >= static_cast<int>(card.atlasHeight))
        {
          continue;
        }

        for (int dx = -1; dx <= 1; ++dx)
        {
          const int neighborX = static_cast<int>(localX) + dx;
          if (neighborX < 0 || neighborX >= static_cast<int>(card.atlasWidth))
          {
            continue;
          }

          const uint32_t neighborIndex = static_cast<uint32_t>(neighborY) * card.atlasWidth + static_cast<uint32_t>(neighborX);
          if (neighborIndex >= tileDepth.size() || neighborIndex >= tileCoverage.size() || tileCoverage[neighborIndex] == 0u)
          {
            continue;
          }

          const float spatialWeight =
              (dx == 0 && dy == 0) ? 4.0f :
              (dx == 0 || dy == 0) ? 2.0f :
              1.0f;
          const float coverageWeight = tileCoverage[neighborIndex] == 1u ? 1.0f : 0.35f;
          const float weight = spatialWeight * coverageWeight;
          weightSum += weight;
          depthSum += static_cast<float>(tileDepth[neighborIndex]) * weight;
        }
      }

      if (weightSum > 0.0f)
      {
        filteredDepth[pixelIndex] = static_cast<uint16_t>(std::round(std::clamp(depthSum / weightSum, 0.0f, 65535.0f)));
      }
    }
  }

  for (uint32_t localY = 0u; localY < card.atlasHeight; ++localY)
  {
    for (uint32_t localX = 0u; localX < card.atlasWidth; ++localX)
    {
      const uint32_t atlasPixelIndex = (card.atlasY + localY) * atlasSize + card.atlasX + localX;
      const uint32_t tilePixelIndex = localY * card.atlasWidth + localX;
      if (atlasPixelIndex >= depthAtlas.size() || atlasPixelIndex >= coverageAtlas.size() || tilePixelIndex >= filteredDepth.size())
      {
        continue;
      }

      if (coverageAtlas[atlasPixelIndex] != 0u)
      {
        depthAtlas[atlasPixelIndex] = filteredDepth[tilePixelIndex];
      }
    }
  }
}

static void buildAtlas(const RBVHBuildOptions &options, RBVHBuildResult &result, const std::vector<RBVHBuildPrimitive> &rasterPrimitiveRefs)
{
  const uint32_t cardCount = static_cast<uint32_t>(result.cards.size());
  if (cardCount == 0u)
  {
    result.tileSize = 0u;
    result.atlasSize = 0u;
    result.cardsPerRow = 0u;
    return;
  }

  const auto sortCardsByResolution =
      [](std::vector<RBVHCardPrimitive> &cards)
      {
        std::sort(
            cards.begin(),
            cards.end(),
            [](const RBVHCardPrimitive &a, const RBVHCardPrimitive &b)
            {
              const uint64_t areaA = static_cast<uint64_t>(a.atlasWidth) * static_cast<uint64_t>(a.atlasHeight);
              const uint64_t areaB = static_cast<uint64_t>(b.atlasWidth) * static_cast<uint64_t>(b.atlasHeight);
              if (areaA != areaB)
              {
                return areaA > areaB;
              }
              if (a.atlasHeight != b.atlasHeight)
              {
                return a.atlasHeight > b.atlasHeight;
              }
              return a.sourcePrimitiveOffset < b.sourcePrimitiveOffset;
            });
      };

  const auto computeCardPixelStats =
      [](const std::vector<RBVHCardPrimitive> &cards, uint64_t &totalCardPixels, uint32_t &maxCardExtent)
      {
        totalCardPixels = 0u;
        maxCardExtent = 1u;
        for (const RBVHCardPrimitive &card : cards)
        {
          const uint32_t cardWidth = std::max(card.atlasWidth, 1u);
          const uint32_t cardHeight = std::max(card.atlasHeight, 1u);
          totalCardPixels += static_cast<uint64_t>(cardWidth) * static_cast<uint64_t>(cardHeight);
          maxCardExtent = std::max(maxCardExtent, std::max(cardWidth, cardHeight));
        }
      };

  const auto downscaleCardsToAtlasBudget =
      [&options, &sortCardsByResolution, &computeCardPixelStats](std::vector<RBVHCardPrimitive> &cards, const uint32_t atlasSize, uint64_t &totalCardPixels, uint32_t &maxCardExtent)
      {
        const uint64_t atlasPixels = static_cast<uint64_t>(atlasSize) * static_cast<uint64_t>(atlasSize);
        if (atlasPixels == 0u || totalCardPixels == 0u)
        {
          return false;
        }

        const double targetPixels = static_cast<double>(atlasPixels) * 0.70;
        double scale = std::sqrt(targetPixels / static_cast<double>(std::max<uint64_t>(totalCardPixels, 1u)));
        if (!(scale < 1.0))
        {
          scale = 0.85;
        }
        scale = std::clamp(scale, 0.25, 0.95);

        bool changed = false;
        for (RBVHCardPrimitive &card : cards)
        {
          const uint32_t oldWidth = std::max(card.atlasWidth, 1u);
          const uint32_t oldHeight = std::max(card.atlasHeight, 1u);
          const uint32_t newWidth = clampCardResolution(static_cast<uint32_t>(std::floor(static_cast<double>(oldWidth) * scale)), options);
          const uint32_t newHeight = clampCardResolution(static_cast<uint32_t>(std::floor(static_cast<double>(oldHeight) * scale)), options);
          if (newWidth < oldWidth || newHeight < oldHeight)
          {
            card.atlasWidth = newWidth;
            card.atlasHeight = newHeight;
            changed = true;
          }
        }

        if (changed)
        {
          sortCardsByResolution(cards);
          computeCardPixelStats(cards, totalCardPixels, maxCardExtent);
        }
        return changed;
      };

  sortCardsByResolution(result.cards);

  uint64_t totalCardPixels = 0u;
  uint32_t maxCardExtent = 1u;
  computeCardPixelStats(result.cards, totalCardPixels, maxCardExtent);

  const uint32_t maxAtlasSize = std::max(options.maxAtlasSize, 1u);
  result.tileSize = maxCardExtent;
  result.atlasSize = nextPowerOfTwo(std::max(maxCardExtent, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(totalCardPixels))))));
  result.atlasSize = std::min(result.atlasSize, maxAtlasSize);
  result.cardsPerRow = result.tileSize > 0u ? result.atlasSize / result.tileSize : 0u;

  bool packed = false;
  while (!packed)
  {
    uint32_t cursorX = 0u;
    uint32_t cursorY = 0u;
    uint32_t rowHeight = 0u;
    packed = true;
    for (RBVHCardPrimitive &card : result.cards)
    {
      const uint32_t tileWidth = std::max(card.atlasWidth, 1u);
      const uint32_t tileHeight = std::max(card.atlasHeight, 1u);
      const uint32_t alignment = std::min(cardMipAlignment(card), result.atlasSize);
      uint32_t alignedX = alignUp(cursorX, alignment);
      uint32_t alignedY = alignUp(cursorY, alignment);
      if (alignedX + tileWidth > result.atlasSize)
      {
        cursorY += rowHeight;
        cursorX = 0u;
        rowHeight = 0u;
        alignedX = 0u;
        alignedY = alignUp(cursorY, alignment);
      }
      if (alignedY + tileHeight > result.atlasSize)
      {
        if (result.atlasSize >= maxAtlasSize)
        {
          if (!downscaleCardsToAtlasBudget(result.cards, result.atlasSize, totalCardPixels, maxCardExtent))
          {
            throw std::runtime_error("RBVH atlas exceeded maxAtlasSize while packing card heightfields");
          }
          result.tileSize = maxCardExtent;
          result.cardsPerRow = result.tileSize > 0u ? result.atlasSize / result.tileSize : 0u;
          packed = false;
          break;
        }
        packed = false;
        result.atlasSize *= 2u;
        result.atlasSize = std::min(result.atlasSize, maxAtlasSize);
        result.cardsPerRow = result.tileSize > 0u ? result.atlasSize / result.tileSize : 0u;
        break;
      }

      card.atlasX = alignedX;
      card.atlasY = alignedY;
      cursorX = alignedX + tileWidth;
      rowHeight = std::max(rowHeight, alignedY - cursorY + tileHeight);
    }
  }

  result.depthAtlasR16.assign(static_cast<size_t>(result.atlasSize) * static_cast<size_t>(result.atlasSize), 0xffffu);
  result.coverageAtlasR8.assign(static_cast<size_t>(result.atlasSize) * static_cast<size_t>(result.atlasSize), 0u);
  result.albedoAtlasRGBA8.clear();

  for (uint32_t cardIndex = 0u; cardIndex < cardCount; ++cardIndex)
  {
    RBVHCardPrimitive &card = result.cards[cardIndex];
    rasterizeCardToAtlas(card, result.atlasSize, rasterPrimitiveRefs, result.depthAtlasR16, result.coverageAtlasR8);
    dilateCardTile(card, result.atlasSize, result.depthAtlasR16, result.coverageAtlasR8, 2u);
    prefilterCardTileDepth(card, result.atlasSize, result.depthAtlasR16, result.coverageAtlasR8);
  }

  result.depthMinMaxMipAtlasRG16 = RBVHBuilder::buildDepthMinMaxMipAtlas(result.atlasSize, result.depthAtlasR16, result.coverageAtlasR8);
  assignCardDepthMipOffsets(result);
}

static void buildCardCwbvh(RBVHBuildResult &result, const RBVHBuildOptions &options)
{
  std::vector<CWBVHBuilder::PrimitiveBounds> cardBounds;
  cardBounds.reserve(result.cards.size());
  for (uint32_t cardIndex = 0u; cardIndex < static_cast<uint32_t>(result.cards.size()); ++cardIndex)
  {
    const RBVHCardPrimitive &card = result.cards[cardIndex];
    const virtualgeometry::AABB bounds = cardTraversalBounds(card, options);
    cardBounds.push_back(
        CWBVHBuilder::PrimitiveBounds{
          .bounds = bounds,
          .primitiveIndex = cardIndex,
        });
  }
  result.cardBuild = CWBVHBuilder::buildFromAabbs(cardBounds);
}

} // namespace

RBVHBuildResult RBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh)
{
  return build(mesh, RBVHBuildOptions{});
}

RBVHBuildResult RBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh, const RBVHBuildOptions &options)
{
  return build(mesh, buildPrimitiveBounds(mesh), options);
}

RBVHBuildResult RBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh, const CWBVHBuilder::BuildResult &triangleBuild, const RBVHBuildOptions &options)
{
  std::vector<CWBVHBuilder::PrimitiveBounds> primitiveBounds = buildPrimitiveBoundsFromTriangleBuild(triangleBuild);
  if (primitiveBounds.empty())
  {
    primitiveBounds = buildPrimitiveBounds(mesh);
  }
  return build(mesh, primitiveBounds, options);
}

RBVHBuildResult RBVHBuilder::build(
    const virtualgeometry::FallbackMeshData &mesh,
    const std::vector<CWBVHBuilder::PrimitiveBounds> &primitiveBounds,
    const RBVHBuildOptions &options)
{
  if (options.sahBinCount < 2u)
  {
    throw std::runtime_error("RBVHBuilder requires at least 2 SAH bins");
  }

  RBVHBuildOptions adjustedOptions = options;
  adjustedOptions.minCardResolution = std::max(options.minCardResolution, 1u);
  adjustedOptions.maxCardResolution = std::max(options.maxCardResolution, adjustedOptions.minCardResolution);
  adjustedOptions.maxAtlasSize = std::max(options.maxAtlasSize, adjustedOptions.maxCardResolution);
  adjustedOptions.maxCards = std::max(options.maxCards, 1u);
  adjustedOptions.texelsPerUvUnit = std::max(options.texelsPerUvUnit, 1.0f);
  adjustedOptions.pixelToAreaRatio = std::max(options.pixelToAreaRatio, 0.0f);
  adjustedOptions.targetTileResolution = std::max(options.targetTileResolution, 0.0f);
  adjustedOptions.projectedAreaThreshold = std::clamp(options.projectedAreaThreshold, 0.0f, 1.0f);
  adjustedOptions.minNormalDot = std::clamp(options.minNormalDot, 0.0f, 1.0f);
  adjustedOptions.adaptiveLodNearDistance = std::max(options.adaptiveLodNearDistance, 0.0f);
  adjustedOptions.adaptiveLodFarDistance = std::max(options.adaptiveLodFarDistance, adjustedOptions.adaptiveLodNearDistance + 1.0e-4f);
  adjustedOptions.adaptiveLodFarMinNormalDot = std::clamp(options.adaptiveLodFarMinNormalDot, 0.0f, 1.0f);
  adjustedOptions.adaptiveLodFarProjectedAreaThreshold = std::clamp(options.adaptiveLodFarProjectedAreaThreshold, 0.0f, 1.0f);
  adjustedOptions.adaptiveLodFarPixelToAreaScale = std::max(options.adaptiveLodFarPixelToAreaScale, 1.0e-4f);
  adjustedOptions.adaptiveLodFarTargetTileResolutionScale = std::max(options.adaptiveLodFarTargetTileResolutionScale, 1.0e-4f);
  adjustedOptions.splitOverlapFraction = std::clamp(options.splitOverlapFraction, 0.0f, 0.25f);
  adjustedOptions.splitStrategy = RBVHSplitStrategy::SpatialMedian;

  RBVHBuildResult result{};
  result.lodLevel = mesh.lodLevel;

  std::vector<RBVHBuildPrimitive> primitives = buildPrimitiveRefs(mesh, primitiveBounds);
  if (primitives.empty())
  {
    return result;
  }

  const RBVHNodeStats rootStats = computeNodeStats(primitives, 0u, static_cast<uint32_t>(primitives.size()));
  if (adjustedOptions.texelsPerWorldUnit <= 0.0f)
  {
    float maxExtent = 0.0f;
    if (rootStats.hasBounds)
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        maxExtent = std::max(maxExtent, rootStats.bounds.maxPoint[axis] - rootStats.bounds.minPoint[axis]);
      }
    }
    adjustedOptions.texelsPerWorldUnit = maxExtent > 1.0e-6f ? static_cast<float>(adjustedOptions.maxCardResolution) / maxExtent : 1.0f;
  }
  if (adjustedOptions.pixelToAreaRatio <= 0.0f)
  {
    const float texelsPerWorldUnit = std::max(adjustedOptions.texelsPerWorldUnit, 1.0f);
    adjustedOptions.pixelToAreaRatio = texelsPerWorldUnit * texelsPerWorldUnit;
  }
  if (adjustedOptions.targetTileResolution <= 0.0f)
  {
    adjustedOptions.targetTileResolution = estimatePaperCoherentTargetTileResolution(adjustedOptions.pixelToAreaRatio, adjustedOptions);
  }

  std::vector<RBVHBuildPrimitive> rasterPrimitiveRefs;
  rasterPrimitiveRefs.reserve(primitives.size());
  buildCardsBreadthFirst(primitives, adjustedOptions, result, rasterPrimitiveRefs);
  buildAtlas(adjustedOptions, result, rasterPrimitiveRefs);
  buildCardCwbvh(result, adjustedOptions);
  return result;
}

std::vector<uint32_t> RBVHBuilder::buildDepthMinMaxMipAtlas(const uint32_t atlasSize, const std::vector<uint16_t> &depthAtlasR16, const std::vector<uint8_t> &coverageAtlasR8)
{
  if (atlasSize <= 1u || depthAtlasR16.size() < static_cast<size_t>(atlasSize) * static_cast<size_t>(atlasSize))
  {
    return {};
  }

  std::vector<uint32_t> current(static_cast<size_t>(atlasSize) * static_cast<size_t>(atlasSize), packDepthMinMax16(0xffffu, 0u));
  for (uint32_t y = 0u; y < atlasSize; ++y)
  {
    for (uint32_t x = 0u; x < atlasSize; ++x)
    {
      const uint32_t index = y * atlasSize + x;
      if (index < coverageAtlasR8.size() && coverageAtlasR8[index] != 0u)
      {
        current[index] = packDepthMinMax16(depthAtlasR16[index], depthAtlasR16[index]);
      }
    }
  }

  std::vector<uint32_t> mipAtlas;
  uint32_t currentSize = atlasSize;
  while (currentSize > 1u)
  {
    const uint32_t nextSize = std::max(1u, currentSize >> 1u);
    std::vector<uint32_t> next(static_cast<size_t>(nextSize) * static_cast<size_t>(nextSize), packDepthMinMax16(0xffffu, 0u));

    for (uint32_t y = 0u; y < nextSize; ++y)
    {
      for (uint32_t x = 0u; x < nextSize; ++x)
      {
        uint32_t merged = packDepthMinMax16(0xffffu, 0u);
        for (uint32_t oy = 0u; oy < 2u; ++oy)
        {
          for (uint32_t ox = 0u; ox < 2u; ++ox)
          {
            const uint32_t sourceX = std::min(currentSize - 1u, x * 2u + ox);
            const uint32_t sourceY = std::min(currentSize - 1u, y * 2u + oy);
            merged = mergeDepthMinMax16(merged, current[sourceY * currentSize + sourceX]);
          }
        }
        next[y * nextSize + x] = merged;
      }
    }

    mipAtlas.insert(mipAtlas.end(), next.begin(), next.end());
    current = std::move(next);
    currentSize = nextSize;
  }

  return mipAtlas;
}

} // namespace softwarebvh
