#include "softwarebvh/CWBVHBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace softwarebvh
{

namespace
{

struct BuildPrimitive
{
  uint32_t primitiveIndex = CWBVHBuilder::INVALID_PRIMITIVE_INDEX;
  uint32_t dominantBoneIndex = UINT32_MAX;
  virtualgeometry::AABB bounds{};
  float centroid[3]{0.0f, 0.0f, 0.0f};
};

struct SAHBin
{
  virtualgeometry::AABB bounds{};
  uint32_t count = 0u;
  bool hasBounds = false;
};

struct BVH8BuildChild
{
  uint32_t canonicalNodeIndex = CWBVHBuilder::INVALID_NODE;
  bool isLeaf = false;
};

struct BVH8SplitCandidate
{
  uint32_t frontierIndex = CWBVHBuilder::INVALID_NODE;
  float costReduction = -std::numeric_limits<float>::max();
  float sourceArea = -1.0f;
};

struct SplitCandidate
{
  uint32_t axis = CWBVHBuilder::INVALID_NODE;
  uint32_t mid = 0u;
  uint32_t binSplit = 0u;
  float cost = std::numeric_limits<float>::infinity();
};

static bool isValidAabb(const virtualgeometry::AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] && bounds.minPoint[1] <= bounds.maxPoint[1] && bounds.minPoint[2] <= bounds.maxPoint[2];
}

static virtualgeometry::AABB unionBounds(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  return virtualgeometry::AABB::Union(a, b);
}

static uint32_t pack4xU8(const uint32_t x, const uint32_t y, const uint32_t z, const uint32_t w)
{
  return (x & 0xffu) | ((y & 0xffu) << 8u) | ((z & 0xffu) << 16u) | ((w & 0xffu) << 24u);
}

static uint32_t bitCastU32(const float value)
{
  uint32_t result = 0u;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

static uint32_t encodeSignedExponent8(const float extent)
{
  if (!(extent > 0.0f) || !std::isfinite(extent))
  {
    return 0u;
  }

  const float step = extent / 255.0f;
  int clampedExponent = std::clamp(static_cast<int>(std::ceil(std::log2(step))), -128, 127);
  while (clampedExponent < 127 && std::ceil(extent / std::exp2(static_cast<float>(clampedExponent))) > 255.0f)
  {
    ++clampedExponent;
  }
  return static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(clampedExponent)));
}

static float decodeExponentScale(const uint32_t encodedExponent)
{
  const int exponent = static_cast<int>(static_cast<int8_t>(encodedExponent & 0xffu));
  return std::exp2(static_cast<float>(exponent));
}

static uint32_t quantizeLowerBound(const float value, const float parentMin, const float scale)
{
  if (!(scale > 0.0f))
  {
    return 0u;
  }

  uint32_t q = static_cast<uint32_t>(std::clamp(std::floor((value - parentMin) / scale), 0.0f, 255.0f));
  if (q > 0u)
  {
    --q;
  }
  return q;
}

static uint32_t quantizeUpperBound(const float value, const float parentMin, const float scale)
{
  if (!(scale > 0.0f))
  {
    return 0u;
  }

  uint32_t q = static_cast<uint32_t>(std::clamp(std::ceil((value - parentMin) / scale), 0.0f, 255.0f));
  if (q < 255u)
  {
    ++q;
  }
  return q;
}

static float aabbSurfaceArea(const virtualgeometry::AABB &bounds)
{
  const float dx = std::max(0.0f, bounds.maxPoint[0] - bounds.minPoint[0]);
  const float dy = std::max(0.0f, bounds.maxPoint[1] - bounds.minPoint[1]);
  const float dz = std::max(0.0f, bounds.maxPoint[2] - bounds.minPoint[2]);
  return 2.0f * (dx * dy + dy * dz + dz * dx);
}

static float aabbSurfaceArea(const float minPoint[3], const float maxPoint[3])
{
  const float dx = std::max(0.0f, maxPoint[0] - minPoint[0]);
  const float dy = std::max(0.0f, maxPoint[1] - minPoint[1]);
  const float dz = std::max(0.0f, maxPoint[2] - minPoint[2]);
  return 2.0f * (dx * dy + dy * dz + dz * dx);
}

static constexpr uint32_t kDobbNodeFlagOriented = 1u << 0u;
static constexpr uint32_t kDobbNodeFlagQuantized = 1u << 1u;
static constexpr uint32_t kDobbFixed104RotationSet = 0u;
static constexpr uint32_t kDobbAxisCount = 13u;
static constexpr uint32_t kDobbAngleCount = 8u;
static constexpr uint32_t kDobbRotationCount = kDobbAxisCount * kDobbAngleCount;
static constexpr uint32_t kDobbPlaneCount = kDobbAxisCount * 2u;
static constexpr float kPi = 3.14159265358979323846f;

struct Vec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

static Vec3 makeVec3(const float x, const float y, const float z)
{
  return Vec3{x, y, z};
}

static Vec3 addVec3(const Vec3 a, const Vec3 b)
{
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3 subVec3(const Vec3 a, const Vec3 b)
{
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 mulVec3(const Vec3 v, const float s)
{
  return Vec3{v.x * s, v.y * s, v.z * s};
}

static Vec3 divVec3(const Vec3 v, const float s)
{
  return Vec3{v.x / s, v.y / s, v.z / s};
}

static float dotVec3(const Vec3 a, const Vec3 b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 crossVec3(const Vec3 a, const Vec3 b)
{
  return Vec3{
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x,
  };
}

static float lengthSqVec3(const Vec3 v)
{
  return dotVec3(v, v);
}

static Vec3 normalizeVec3(const Vec3 v)
{
  const float lengthSq = dotVec3(v, v);
  if (!(lengthSq > 0.0f))
  {
    return Vec3{1.0f, 0.0f, 0.0f};
  }
  return mulVec3(v, 1.0f / std::sqrt(lengthSq));
}

static Vec3 safeNormalizeVec3(const Vec3 v, const Vec3 fallback)
{
  const float lengthSq = dotVec3(v, v);
  if (!(lengthSq > 1.0e-20f))
  {
    return fallback;
  }
  return mulVec3(v, 1.0f / std::sqrt(lengthSq));
}

static Vec3 dobbAxis(const uint32_t axisIndex)
{
  static constexpr Vec3 kAxes[kDobbAxisCount] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
    {1.0f, 1.0f, 0.0f},
    {1.0f, -1.0f, 0.0f},
    {1.0f, 0.0f, 1.0f},
    {1.0f, 0.0f, -1.0f},
    {0.0f, 1.0f, 1.0f},
    {0.0f, 1.0f, -1.0f},
    {1.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, -1.0f},
    {1.0f, -1.0f, 1.0f},
    {-1.0f, 1.0f, 1.0f},
  };
  return normalizeVec3(kAxes[axisIndex % kDobbAxisCount]);
}

static Vec3 dobbPlaneAxis(const uint32_t planeIndex)
{
  const Vec3 axis = dobbAxis(planeIndex % kDobbAxisCount);
  return planeIndex < kDobbAxisCount ? axis : mulVec3(axis, -1.0f);
}

static Vec3 rotateDobbVector(const Vec3 value, const uint32_t rotationIndex)
{
  if (rotationIndex == 0u)
  {
    return value;
  }

  const uint32_t axisIndex = (rotationIndex / kDobbAngleCount) % kDobbAxisCount;
  const uint32_t angleIndex = rotationIndex % kDobbAngleCount;
  if (angleIndex == 0u)
  {
    return value;
  }

  const Vec3 axis = dobbAxis(axisIndex);
  const float angle = static_cast<float>(angleIndex) * (kPi / static_cast<float>(kDobbAngleCount));
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  return addVec3(addVec3(mulVec3(value, c), mulVec3(crossVec3(axis, value), s)), mulVec3(axis, dotVec3(axis, value) * (1.0f - c)));
}

static Vec3 inverseRotateDobbVector(const Vec3 value, const uint32_t rotationIndex)
{
  if (rotationIndex == 0u)
  {
    return value;
  }

  const uint32_t axisIndex = (rotationIndex / kDobbAngleCount) % kDobbAxisCount;
  const uint32_t angleIndex = rotationIndex % kDobbAngleCount;
  if (angleIndex == 0u)
  {
    return value;
  }

  const Vec3 axis = dobbAxis(axisIndex);
  const float angle = -static_cast<float>(angleIndex) * (kPi / static_cast<float>(kDobbAngleCount));
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  return addVec3(addVec3(mulVec3(value, c), mulVec3(crossVec3(axis, value), s)), mulVec3(axis, dotVec3(axis, value) * (1.0f - c)));
}

static virtualgeometry::AABB emptyAabb()
{
  virtualgeometry::AABB bounds{};
  bounds.minPoint[0] = std::numeric_limits<float>::infinity();
  bounds.minPoint[1] = std::numeric_limits<float>::infinity();
  bounds.minPoint[2] = std::numeric_limits<float>::infinity();
  bounds.maxPoint[0] = -std::numeric_limits<float>::infinity();
  bounds.maxPoint[1] = -std::numeric_limits<float>::infinity();
  bounds.maxPoint[2] = -std::numeric_limits<float>::infinity();
  return bounds;
}

static void includePoint(virtualgeometry::AABB &bounds, const Vec3 point)
{
  bounds.minPoint[0] = std::min(bounds.minPoint[0], point.x);
  bounds.minPoint[1] = std::min(bounds.minPoint[1], point.y);
  bounds.minPoint[2] = std::min(bounds.minPoint[2], point.z);
  bounds.maxPoint[0] = std::max(bounds.maxPoint[0], point.x);
  bounds.maxPoint[1] = std::max(bounds.maxPoint[1], point.y);
  bounds.maxPoint[2] = std::max(bounds.maxPoint[2], point.z);
}

static virtualgeometry::AABB rotateAabb(const virtualgeometry::AABB &bounds, const uint32_t rotationIndex)
{
  virtualgeometry::AABB rotated = emptyAabb();
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const Vec3 point = makeVec3(
        (corner & 1u) != 0u ? bounds.maxPoint[0] : bounds.minPoint[0],
        (corner & 2u) != 0u ? bounds.maxPoint[1] : bounds.minPoint[1],
        (corner & 4u) != 0u ? bounds.maxPoint[2] : bounds.minPoint[2]);
    includePoint(rotated, rotateDobbVector(point, rotationIndex));
  }
  return rotated;
}

static float axisAlignmentScore(const Vec3 axis)
{
  return std::max({std::fabs(axis.x), std::fabs(axis.y), std::fabs(axis.z)});
}

static uint32_t closestDobbRotationForBasis(const Vec3 a0, const Vec3 a1, const Vec3 a2)
{
  float bestScore = axisAlignmentScore(a0) + axisAlignmentScore(a1) + axisAlignmentScore(a2);
  uint32_t bestRotationIndex = 0u;
  for (uint32_t rotationIndex = 1u; rotationIndex < kDobbRotationCount; ++rotationIndex)
  {
    if ((rotationIndex % kDobbAngleCount) == 0u)
    {
      continue;
    }

    const float score =
        axisAlignmentScore(rotateDobbVector(a0, rotationIndex)) +
        axisAlignmentScore(rotateDobbVector(a1, rotationIndex)) +
        axisAlignmentScore(rotateDobbVector(a2, rotationIndex));
    if (score > bestScore)
    {
      bestScore = score;
      bestRotationIndex = rotationIndex;
    }
  }
  return bestRotationIndex;
}

static virtualgeometry::AABB unionBoundsLocal(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  virtualgeometry::AABB result{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    result.minPoint[axis] = std::min(a.minPoint[axis], b.minPoint[axis]);
    result.maxPoint[axis] = std::max(a.maxPoint[axis], b.maxPoint[axis]);
  }
  return result;
}

struct DobbApexFacet
{
  uint32_t planeIndex[3]{};
  Vec3 normal{};
  float offset = 0.0f;
};

static float signedPlaneDistanceForProxy(const CWBVHBuilder::DobbProjectionProxy &proxy, const uint32_t planeIndex)
{
  const uint32_t axisIndex = planeIndex % kDobbAxisCount;
  return planeIndex < kDobbAxisCount ? proxy.maxProjection[axisIndex] : -proxy.minProjection[axisIndex];
}

static CWBVHBuilder::DobbProjectionProxy emptyDobbProjectionProxy()
{
  CWBVHBuilder::DobbProjectionProxy proxy{};
  proxy.valid = 0u;
  for (uint32_t axisIndex = 0u; axisIndex < kDobbAxisCount; ++axisIndex)
  {
    proxy.minProjection[axisIndex] = std::numeric_limits<float>::infinity();
    proxy.maxProjection[axisIndex] = -std::numeric_limits<float>::infinity();
  }
  return proxy;
}

static void includePointInDobbProjectionProxy(CWBVHBuilder::DobbProjectionProxy &proxy, const Vec3 point)
{
  proxy.valid = 1u;
  for (uint32_t axisIndex = 0u; axisIndex < kDobbAxisCount; ++axisIndex)
  {
    const float projection = dotVec3(dobbAxis(axisIndex), point);
    proxy.minProjection[axisIndex] = std::min(proxy.minProjection[axisIndex], projection);
    proxy.maxProjection[axisIndex] = std::max(proxy.maxProjection[axisIndex], projection);
  }
}

static CWBVHBuilder::DobbProjectionProxy mergeDobbProjectionProxy(const CWBVHBuilder::DobbProjectionProxy &a, const CWBVHBuilder::DobbProjectionProxy &b)
{
  if (a.valid == 0u)
  {
    return b;
  }
  if (b.valid == 0u)
  {
    return a;
  }

  CWBVHBuilder::DobbProjectionProxy result{};
  result.valid = 1u;
  for (uint32_t axisIndex = 0u; axisIndex < kDobbAxisCount; ++axisIndex)
  {
    result.minProjection[axisIndex] = std::min(a.minProjection[axisIndex], b.minProjection[axisIndex]);
    result.maxProjection[axisIndex] = std::max(a.maxProjection[axisIndex], b.maxProjection[axisIndex]);
  }
  return result;
}

static CWBVHBuilder::DobbProjectionProxy computeAabbDobbProjectionProxy(const virtualgeometry::AABB &bounds)
{
  CWBVHBuilder::DobbProjectionProxy proxy = emptyDobbProjectionProxy();
  if (!isValidAabb(bounds))
  {
    return proxy;
  }

  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    includePointInDobbProjectionProxy(
        proxy,
        makeVec3(
            (corner & 1u) != 0u ? bounds.maxPoint[0] : bounds.minPoint[0],
            (corner & 2u) != 0u ? bounds.maxPoint[1] : bounds.minPoint[1],
            (corner & 4u) != 0u ? bounds.maxPoint[2] : bounds.minPoint[2]));
  }
  return proxy;
}

static float determinant3(const Vec3 a, const Vec3 b, const Vec3 c)
{
  return dotVec3(a, crossVec3(b, c));
}

static bool solvePlaneIntersection(const Vec3 n0, const float d0, const Vec3 n1, const float d1, const Vec3 n2, const float d2, Vec3 &outPoint)
{
  const float det = determinant3(n0, n1, n2);
  if (std::fabs(det) <= 1.0e-7f)
  {
    return false;
  }

  outPoint = divVec3(
      addVec3(
          addVec3(mulVec3(crossVec3(n1, n2), d0), mulVec3(crossVec3(n2, n0), d1)),
          mulVec3(crossVec3(n0, n1), d2)),
      det);
  return std::isfinite(outPoint.x) && std::isfinite(outPoint.y) && std::isfinite(outPoint.z);
}

static bool pointInTriangle(const Vec3 point, const Vec3 a, const Vec3 b, const Vec3 c)
{
  const Vec3 v0 = subVec3(c, a);
  const Vec3 v1 = subVec3(b, a);
  const Vec3 v2 = subVec3(point, a);
  const float dot00 = dotVec3(v0, v0);
  const float dot01 = dotVec3(v0, v1);
  const float dot02 = dotVec3(v0, v2);
  const float dot11 = dotVec3(v1, v1);
  const float dot12 = dotVec3(v1, v2);
  const float denom = dot00 * dot11 - dot01 * dot01;
  if (std::fabs(denom) <= 1.0e-8f)
  {
    return false;
  }

  const float invDenom = 1.0f / denom;
  const float u = (dot11 * dot02 - dot01 * dot12) * invDenom;
  const float v = (dot00 * dot12 - dot01 * dot02) * invDenom;
  return u >= -1.0e-4f && v >= -1.0e-4f && (u + v) <= 1.0001f;
}

static const std::vector<DobbApexFacet> &dobbApexFacets()
{
  static const std::vector<DobbApexFacet> facets = []
  {
    std::vector<DobbApexFacet> result;
    for (uint32_t i = 0u; i < kDobbPlaneCount; ++i)
    {
      const Vec3 pi = dobbPlaneAxis(i);
      for (uint32_t j = i + 1u; j < kDobbPlaneCount; ++j)
      {
        const Vec3 pj = dobbPlaneAxis(j);
        for (uint32_t k = j + 1u; k < kDobbPlaneCount; ++k)
        {
          const Vec3 pk = dobbPlaneAxis(k);
          Vec3 normal = crossVec3(subVec3(pj, pi), subVec3(pk, pi));
          float normalLengthSq = lengthSqVec3(normal);
          if (normalLengthSq <= 1.0e-10f)
          {
            continue;
          }
          normal = mulVec3(normal, 1.0f / std::sqrt(normalLengthSq));
          float offset = dotVec3(normal, pi);
          if (offset < 0.0f)
          {
            normal = mulVec3(normal, -1.0f);
            offset = -offset;
          }
          if (offset <= 1.0e-5f)
          {
            continue;
          }

          bool isFacet = true;
          for (uint32_t pointIndex = 0u; pointIndex < kDobbPlaneCount; ++pointIndex)
          {
            const float distance = dotVec3(normal, dobbPlaneAxis(pointIndex)) - offset;
            if (distance > 1.0e-4f)
            {
              isFacet = false;
              break;
            }
          }
          if (!isFacet)
          {
            continue;
          }

          result.push_back(
              DobbApexFacet{
                .planeIndex = {i, j, k},
                .normal = normal,
                .offset = offset,
              });
        }
      }
    }
    return result;
  }();
  return facets;
}

static const DobbApexFacet *findDobbApexFacet(const Vec3 direction)
{
  const Vec3 normalizedDirection = safeNormalizeVec3(direction, makeVec3(1.0f, 0.0f, 0.0f));
  const DobbApexFacet *bestFallback = nullptr;
  float bestNormalAlignment = -std::numeric_limits<float>::infinity();
  for (const DobbApexFacet &facet : dobbApexFacets())
  {
    const float normalAlignment = dotVec3(facet.normal, normalizedDirection);
    if (normalAlignment > bestNormalAlignment)
    {
      bestNormalAlignment = normalAlignment;
      bestFallback = &facet;
    }

    if (normalAlignment <= 1.0e-6f)
    {
      continue;
    }

    const float t = facet.offset / normalAlignment;
    if (!(t > 0.0f) || !std::isfinite(t))
    {
      continue;
    }

    const Vec3 point = mulVec3(normalizedDirection, t);
    const Vec3 p0 = dobbPlaneAxis(facet.planeIndex[0u]);
    const Vec3 p1 = dobbPlaneAxis(facet.planeIndex[1u]);
    const Vec3 p2 = dobbPlaneAxis(facet.planeIndex[2u]);
    if (pointInTriangle(point, p0, p1, p2))
    {
      return &facet;
    }
  }
  return bestFallback;
}

static float supportDobbProjectionProxyExact(const CWBVHBuilder::DobbProjectionProxy &proxy, const Vec3 direction)
{
  float best = -std::numeric_limits<float>::infinity();
  for (uint32_t i = 0u; i < kDobbPlaneCount; ++i)
  {
    const Vec3 n0 = dobbPlaneAxis(i);
    const float d0 = signedPlaneDistanceForProxy(proxy, i);
    for (uint32_t j = i + 1u; j < kDobbPlaneCount; ++j)
    {
      const Vec3 n1 = dobbPlaneAxis(j);
      const float d1 = signedPlaneDistanceForProxy(proxy, j);
      for (uint32_t k = j + 1u; k < kDobbPlaneCount; ++k)
      {
        const Vec3 n2 = dobbPlaneAxis(k);
        const float d2 = signedPlaneDistanceForProxy(proxy, k);
        Vec3 point{};
        if (!solvePlaneIntersection(n0, d0, n1, d1, n2, d2, point))
        {
          continue;
        }

        bool inside = true;
        for (uint32_t planeIndex = 0u; planeIndex < kDobbPlaneCount; ++planeIndex)
        {
          const float distance = signedPlaneDistanceForProxy(proxy, planeIndex);
          if (dotVec3(dobbPlaneAxis(planeIndex), point) > distance + 1.0e-4f)
          {
            inside = false;
            break;
          }
        }
        if (inside)
        {
          best = std::max(best, dotVec3(direction, point));
        }
      }
    }
  }
  return best;
}

static float supportDobbProjectionProxyAabbFallback(const CWBVHBuilder::DobbProjectionProxy &proxy, const Vec3 direction)
{
  const float x = direction.x >= 0.0f ? proxy.maxProjection[0u] : proxy.minProjection[0u];
  const float y = direction.y >= 0.0f ? proxy.maxProjection[1u] : proxy.minProjection[1u];
  const float z = direction.z >= 0.0f ? proxy.maxProjection[2u] : proxy.minProjection[2u];
  return direction.x * x + direction.y * y + direction.z * z;
}

static float supportDobbProjectionProxy(const CWBVHBuilder::DobbProjectionProxy &proxy, const Vec3 direction)
{
  if (proxy.valid == 0u)
  {
    return 0.0f;
  }

  const Vec3 normalizedDirection = safeNormalizeVec3(direction, makeVec3(1.0f, 0.0f, 0.0f));
  float apexSupport = -std::numeric_limits<float>::infinity();
  const DobbApexFacet *facet = findDobbApexFacet(normalizedDirection);
  if (facet != nullptr)
  {
    const Vec3 n0 = dobbPlaneAxis(facet->planeIndex[0u]);
    const Vec3 n1 = dobbPlaneAxis(facet->planeIndex[1u]);
    const Vec3 n2 = dobbPlaneAxis(facet->planeIndex[2u]);
    const float d0 = signedPlaneDistanceForProxy(proxy, facet->planeIndex[0u]);
    const float d1 = signedPlaneDistanceForProxy(proxy, facet->planeIndex[1u]);
    const float d2 = signedPlaneDistanceForProxy(proxy, facet->planeIndex[2u]);
    Vec3 apexPoint{};
    if (solvePlaneIntersection(n0, d0, n1, d1, n2, d2, apexPoint))
    {
      const float support = dotVec3(direction, apexPoint);
      if (std::isfinite(support))
      {
        apexSupport = support;
      }
    }
  }

  if (std::isfinite(apexSupport))
  {
    return apexSupport;
  }
  return supportDobbProjectionProxyAabbFallback(proxy, direction);
}

static virtualgeometry::AABB refitDobbProjectionProxyInRotatedSpace(const CWBVHBuilder::DobbProjectionProxy &proxy, const uint32_t rotationIndex)
{
  if (proxy.valid == 0u)
  {
    return virtualgeometry::AABB{};
  }

  static constexpr Vec3 kOutputAxes[3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
  };

  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const Vec3 sourceAxis = inverseRotateDobbVector(kOutputAxes[axis], rotationIndex);
    const float maxProjection = supportDobbProjectionProxy(proxy, sourceAxis);
    const float minProjection = -supportDobbProjectionProxy(proxy, mulVec3(sourceAxis, -1.0f));
    bounds.minPoint[axis] = std::nextafter(minProjection, -std::numeric_limits<float>::infinity());
    bounds.maxPoint[axis] = std::nextafter(maxProjection, std::numeric_limits<float>::infinity());
  }
  return bounds;
}

static virtualgeometry::AABB buildLocalBounds(const virtualgeometry::FallbackMeshData &mesh)
{
  virtualgeometry::AABB bounds{};
  bounds.minPoint[0] = mesh.boundsMin[0];
  bounds.minPoint[1] = mesh.boundsMin[1];
  bounds.minPoint[2] = mesh.boundsMin[2];
  bounds.maxPoint[0] = mesh.boundsMax[0];
  bounds.maxPoint[1] = mesh.boundsMax[1];
  bounds.maxPoint[2] = mesh.boundsMax[2];
  if (isValidAabb(bounds))
  {
    return bounds;
  }

  for (const virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    bounds.expandBy(vertex.position);
  }
  return bounds;
}

static virtualgeometry::AABB computeTriangleBounds(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return virtualgeometry::AABB{};
  }

  virtualgeometry::AABB bounds{};
  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return virtualgeometry::AABB{};
    }

    bounds.expandBy(mesh.vertices[vertexIndex].position);
  }

  return bounds;
}

static uint32_t computeTriangleDobbRotationIndex(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return 0u;
  }

  Vec3 vertices[3]{};
  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return 0u;
    }
    const virtualgeometry::FallbackMeshVertex &vertex = mesh.vertices[vertexIndex];
    vertices[corner] = makeVec3(vertex.position[0], vertex.position[1], vertex.position[2]);
  }

  struct Edge
  {
    Vec3 vector{};
    float lengthSq = 0.0f;
  };

  std::array<Edge, 3u> edges = {
    Edge{subVec3(vertices[1u], vertices[0u]), lengthSqVec3(subVec3(vertices[1u], vertices[0u]))},
    Edge{subVec3(vertices[2u], vertices[1u]), lengthSqVec3(subVec3(vertices[2u], vertices[1u]))},
    Edge{subVec3(vertices[0u], vertices[2u]), lengthSqVec3(subVec3(vertices[0u], vertices[2u]))},
  };
  std::sort(
      edges.begin(),
      edges.end(),
      [](const Edge &a, const Edge &b)
      {
        return a.lengthSq < b.lengthSq;
      });

  if (edges[0u].lengthSq <= 1e-12f || edges[1u].lengthSq <= 1e-12f)
  {
    return 0u;
  }

  const Vec3 a0 = normalizeVec3(edges[1u].vector);
  Vec3 a1 = normalizeVec3(edges[0u].vector);
  Vec3 a2 = normalizeVec3(crossVec3(a0, a1));
  if (lengthSqVec3(a2) <= 1e-12f)
  {
    return 0u;
  }
  a1 = normalizeVec3(crossVec3(a2, a0));
  return closestDobbRotationForBasis(a0, a1, a2);
}

static CWBVHBuilder::DobbProjectionProxy computeTriangleDobbProjectionProxy(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  CWBVHBuilder::DobbProjectionProxy proxy = emptyDobbProjectionProxy();
  if (indexBase + 2u >= mesh.indices.size())
  {
    return proxy;
  }

  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return emptyDobbProjectionProxy();
    }
    const virtualgeometry::FallbackMeshVertex &vertex = mesh.vertices[vertexIndex];
    includePointInDobbProjectionProxy(proxy, makeVec3(vertex.position[0], vertex.position[1], vertex.position[2]));
  }
  return proxy;
}

static void computeCentroid(const virtualgeometry::AABB &bounds, float outCentroid[3])
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outCentroid[axis] = (bounds.minPoint[axis] + bounds.maxPoint[axis]) * 0.5f;
  }
}

static void orderBVH8Children(std::vector<BVH8BuildChild> &frontier, const std::vector<CWBVHBuilder::CanonicalNode> &canonicalNodes, const virtualgeometry::AABB &parentBounds)
{
  if (frontier.empty())
  {
    return;
  }

  const float parentCenter[3] = {
    (parentBounds.minPoint[0] + parentBounds.maxPoint[0]) * 0.5f,
    (parentBounds.minPoint[1] + parentBounds.maxPoint[1]) * 0.5f,
    (parentBounds.minPoint[2] + parentBounds.maxPoint[2]) * 0.5f,
  };

  float cost[8][8]{};
  for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
  {
    const virtualgeometry::AABB &childBounds = canonicalNodes[frontier[childIndex].canonicalNodeIndex].bounds;
    const float childCenter[3] = {
      (childBounds.minPoint[0] + childBounds.maxPoint[0]) * 0.5f,
      (childBounds.minPoint[1] + childBounds.maxPoint[1]) * 0.5f,
      (childBounds.minPoint[2] + childBounds.maxPoint[2]) * 0.5f,
    };

    for (uint32_t slot = 0u; slot < 8u; ++slot)
    {
      const float sx = (slot & 4u) != 0u ? 1.0f : -1.0f;
      const float sy = (slot & 2u) != 0u ? 1.0f : -1.0f;
      const float sz = (slot & 1u) != 0u ? 1.0f : -1.0f;
      const float vx = childCenter[0] - parentCenter[0];
      const float vy = childCenter[1] - parentCenter[1];
      const float vz = childCenter[2] - parentCenter[2];
      cost[childIndex][slot] = -(vx * sx + vy * sy + vz * sz);
    }
  }

  int assignment[8];
  bool slotFilled[8]{};
  std::fill(std::begin(assignment), std::end(assignment), -1);

  for (uint32_t iteration = 0u; iteration < frontier.size() && iteration < 8u; ++iteration)
  {
    int bestChild = -1;
    int bestSlot = -1;
    float bestCost = 0.0f;
    bool hasBest = false;
    for (uint32_t childIndex = 0u; childIndex < frontier.size() && childIndex < 8u; ++childIndex)
    {
      if (assignment[childIndex] >= 0)
      {
        continue;
      }

      for (uint32_t slot = 0u; slot < 8u; ++slot)
      {
        if (slotFilled[slot])
        {
          continue;
        }

        if (!hasBest || cost[childIndex][slot] < bestCost)
        {
          bestChild = static_cast<int>(childIndex);
          bestSlot = static_cast<int>(slot);
          bestCost = cost[childIndex][slot];
          hasBest = true;
        }
      }
    }

    if (bestChild < 0 || bestSlot < 0)
    {
      break;
    }

    assignment[bestChild] = bestSlot;
    slotFilled[bestSlot] = true;
  }

  std::vector<BVH8BuildChild> ordered(8u);
  for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
  {
    if (assignment[childIndex] >= 0)
    {
      ordered[static_cast<uint32_t>(assignment[childIndex])] = frontier[childIndex];
    }
  }
  frontier = std::move(ordered);
}

static BVH8SplitCandidate chooseBVH8SplitCandidate(const std::vector<BVH8BuildChild> &frontier, const std::vector<CWBVHBuilder::CanonicalNode> &canonicalNodes)
{
  BVH8SplitCandidate best{};

  for (uint32_t index = 0u; index < static_cast<uint32_t>(frontier.size()); ++index)
  {
    if (frontier[index].isLeaf)
    {
      continue;
    }

    const CWBVHBuilder::CanonicalNode &node = canonicalNodes[frontier[index].canonicalNodeIndex];
    if (node.left == CWBVHBuilder::INVALID_NODE || node.right == CWBVHBuilder::INVALID_NODE)
    {
      continue;
    }

    const CWBVHBuilder::CanonicalNode &left = canonicalNodes[node.left];
    const CWBVHBuilder::CanonicalNode &right = canonicalNodes[node.right];
    const float sourceArea = aabbSurfaceArea(node.bounds);
    const float sourceCost = sourceArea * static_cast<float>(node.subtreePrimitiveCount);
    const float splitCost = aabbSurfaceArea(left.bounds) * static_cast<float>(left.subtreePrimitiveCount) + aabbSurfaceArea(right.bounds) * static_cast<float>(right.subtreePrimitiveCount);
    const float costReduction = sourceCost - splitCost;

    if (best.frontierIndex == CWBVHBuilder::INVALID_NODE || costReduction > best.costReduction || (costReduction == best.costReduction && sourceArea > best.sourceArea))
    {
      best.frontierIndex = index;
      best.costReduction = costReduction;
      best.sourceArea = sourceArea;
    }
  }

  return best;
}

static void emitBVH8Recursive(
    const std::vector<CWBVHBuilder::CanonicalNode> &canonicalNodes,
    const uint32_t canonicalNodeIndex,
    const uint32_t bvh8NodeIndex,
    std::vector<CWBVHBuilder::BVH8Node> &outNodes,
    std::vector<uint32_t> &outLeafRemap)
{
  struct PendingNode
  {
    uint32_t canonicalNodeIndex = CWBVHBuilder::INVALID_NODE;
    uint32_t bvh8NodeIndex = CWBVHBuilder::INVALID_NODE;
  };

  std::vector<PendingNode> pending;
  pending.push_back(
      PendingNode{
        .canonicalNodeIndex = canonicalNodeIndex,
        .bvh8NodeIndex = bvh8NodeIndex,
      });

  for (uint32_t pendingIndex = 0u; pendingIndex < static_cast<uint32_t>(pending.size()); ++pendingIndex)
  {
    const PendingNode pendingNode = pending[pendingIndex];
    if (pendingNode.canonicalNodeIndex >= canonicalNodes.size())
    {
      continue;
    }

    if (pendingNode.bvh8NodeIndex >= outNodes.size())
    {
      outNodes.resize(pendingNode.bvh8NodeIndex + 1u);
    }

    const CWBVHBuilder::CanonicalNode &sourceNode = canonicalNodes[pendingNode.canonicalNodeIndex];
    std::vector<BVH8BuildChild> frontier;
    frontier.reserve(8u);

    if (sourceNode.isLeaf)
    {
      frontier.push_back(
          BVH8BuildChild{
            .canonicalNodeIndex = pendingNode.canonicalNodeIndex,
            .isLeaf = true,
          });
    }
    else
    {
      if (sourceNode.left != CWBVHBuilder::INVALID_NODE)
      {
        frontier.push_back(
            BVH8BuildChild{
              .canonicalNodeIndex = sourceNode.left,
              .isLeaf = canonicalNodes[sourceNode.left].isLeaf,
            });
      }
      if (sourceNode.right != CWBVHBuilder::INVALID_NODE)
      {
        frontier.push_back(
            BVH8BuildChild{
              .canonicalNodeIndex = sourceNode.right,
              .isLeaf = canonicalNodes[sourceNode.right].isLeaf,
            });
      }
    }

    while (frontier.size() < 8u)
    {
      uint32_t splitIndex = chooseBVH8SplitCandidate(frontier, canonicalNodes).frontierIndex;

      if (splitIndex == CWBVHBuilder::INVALID_NODE)
      {
        break;
      }

      const BVH8BuildChild childToSplit = frontier[splitIndex];
      const CWBVHBuilder::CanonicalNode &splitNode = canonicalNodes[childToSplit.canonicalNodeIndex];
      frontier.erase(frontier.begin() + splitIndex);

      if (splitNode.left != CWBVHBuilder::INVALID_NODE)
      {
        frontier.insert(
            frontier.begin() + splitIndex,
            BVH8BuildChild{
              .canonicalNodeIndex = splitNode.left,
              .isLeaf = canonicalNodes[splitNode.left].isLeaf,
            });
        ++splitIndex;
      }
      if (splitNode.right != CWBVHBuilder::INVALID_NODE)
      {
        frontier.insert(
            frontier.begin() + splitIndex,
            BVH8BuildChild{
              .canonicalNodeIndex = splitNode.right,
              .isLeaf = canonicalNodes[splitNode.right].isLeaf,
            });
      }
    }

    orderBVH8Children(frontier, canonicalNodes, sourceNode.bounds);

    const uint32_t internalBaseIndex = static_cast<uint32_t>(outNodes.size());
    const uint32_t leafBaseIndex = static_cast<uint32_t>(outLeafRemap.size());
    std::vector<BVH8BuildChild> internalChildren;
    internalChildren.reserve(frontier.size());

    CWBVHBuilder::BVH8Node node{};
    node.bounds = sourceNode.bounds;
    node.cwbvhOutputIndex = pendingNode.bvh8NodeIndex;
    node.internalChildBase = internalBaseIndex;
    node.leafBase = leafBaseIndex;

    uint32_t leafOffset = 0u;
    for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
    {
      const BVH8BuildChild &child = frontier[childSlot];
      if (child.canonicalNodeIndex == CWBVHBuilder::INVALID_NODE)
      {
        continue;
      }

      const CWBVHBuilder::CanonicalNode &childNode = canonicalNodes[child.canonicalNodeIndex];
      node.validChildMask |= 1u << childSlot;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        node.childMin[childSlot][axis] = childNode.bounds.minPoint[axis];
        node.childMax[childSlot][axis] = childNode.bounds.maxPoint[axis];
      }

      if (!child.isLeaf)
      {
        node.internalChildMask |= 1u << childSlot;
        node.childMeta[childSlot] = CWBVHBuilder::BVH8_CHILD_VALID;
        node.childIndex[childSlot] = internalBaseIndex + static_cast<uint32_t>(internalChildren.size());
        internalChildren.push_back(child);
      }
      else
      {
        node.leafChildMask |= 1u << childSlot;
        node.childMeta[childSlot] = CWBVHBuilder::BVH8_CHILD_VALID | CWBVHBuilder::BVH8_CHILD_LEAF;
        node.childIndex[childSlot] = leafBaseIndex + leafOffset;
        outLeafRemap.push_back(childNode.primitiveIndex);
        ++leafOffset;
      }
    }

    outNodes[pendingNode.bvh8NodeIndex] = node;

    if (!internalChildren.empty())
    {
      outNodes.resize(outNodes.size() + internalChildren.size());
      for (uint32_t internalIndex = 0u; internalIndex < static_cast<uint32_t>(internalChildren.size()); ++internalIndex)
      {
        pending.push_back(
            PendingNode{
              .canonicalNodeIndex = internalChildren[internalIndex].canonicalNodeIndex,
              .bvh8NodeIndex = internalBaseIndex + internalIndex,
            });
      }
    }
  }
}

static uint32_t computeDominantBoneIndex(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return UINT32_MAX;
  }

  struct BoneWeightSum
  {
    uint32_t boneIndex = UINT32_MAX;
    float weight = 0.0f;
  };

  BoneWeightSum best{};
  std::vector<BoneWeightSum> sums;
  sums.reserve(12u);

  for (uint32_t corner = 0u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[indexBase + corner];
    if (vertexIndex >= mesh.vertices.size())
    {
      return UINT32_MAX;
    }

    const virtualgeometry::FallbackMeshVertex &vertex = mesh.vertices[vertexIndex];
    for (uint32_t influenceIndex = 0u; influenceIndex < 4u; ++influenceIndex)
    {
      const float weight = vertex.boneWeights[influenceIndex];
      if (weight <= 0.0f)
      {
        continue;
      }

      const uint32_t boneIndex = vertex.boneIndices[influenceIndex];
      auto existing = std::find_if(
          sums.begin(),
          sums.end(),
          [boneIndex](const BoneWeightSum &sum)
          {
            return sum.boneIndex == boneIndex;
          });
      if (existing == sums.end())
      {
        sums.push_back(
            BoneWeightSum{
              .boneIndex = boneIndex,
              .weight = weight,
            });
        existing = std::prev(sums.end());
      }
      else
      {
        existing->weight += weight;
      }

      if (existing->weight > best.weight || (existing->weight == best.weight && existing->boneIndex < best.boneIndex))
      {
        best = *existing;
      }
    }
  }

  return best.weight > 0.0f ? best.boneIndex : UINT32_MAX;
}

static bool meshUsesSkinning(const virtualgeometry::FallbackMeshData &mesh)
{
  if (!mesh.skeleton.empty())
  {
    return true;
  }

  for (const virtualgeometry::FallbackMeshVertex &vertex : mesh.vertices)
  {
    for (const float weight : vertex.boneWeights)
    {
      if (weight > 0.0f)
      {
        return true;
      }
    }
  }
  return false;
}

static bool buildTriangleTransform(const virtualgeometry::FallbackMeshData &mesh, const uint32_t primitiveIndex, CWBVHBuilder::TriBaryTransform &outTransform)
{
  const uint32_t indexBase = primitiveIndex * 3u;
  if (indexBase + 2u >= mesh.indices.size())
  {
    return false;
  }

  const uint32_t i0 = mesh.indices[indexBase + 0u];
  const uint32_t i1 = mesh.indices[indexBase + 1u];
  const uint32_t i2 = mesh.indices[indexBase + 2u];
  if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
  {
    return false;
  }

  const virtualgeometry::FallbackMeshVertex &v0 = mesh.vertices[i0];
  const virtualgeometry::FallbackMeshVertex &v1 = mesh.vertices[i1];
  const virtualgeometry::FallbackMeshVertex &v2 = mesh.vertices[i2];

  const float e1[3] = {
    v1.position[0] - v0.position[0],
    v1.position[1] - v0.position[1],
    v1.position[2] - v0.position[2],
  };
  const float e2[3] = {
    v2.position[0] - v0.position[0],
    v2.position[1] - v0.position[1],
    v2.position[2] - v0.position[2],
  };
  const float n[3] = {
    e1[1] * e2[2] - e1[2] * e2[1],
    e1[2] * e2[0] - e1[0] * e2[2],
    e1[0] * e2[1] - e1[1] * e2[0],
  };

  const float det = e1[0] * (e2[1] * n[2] - e2[2] * n[1]) - e2[0] * (e1[1] * n[2] - e1[2] * n[1]) + n[0] * (e1[1] * e2[2] - e1[2] * e2[1]);
  if (std::fabs(det) <= 1e-20f)
  {
    return false;
  }

  const float invDet = 1.0f / det;
  const float row0[3] = {
    (e2[1] * n[2] - e2[2] * n[1]) * invDet,
    (e2[2] * n[0] - e2[0] * n[2]) * invDet,
    (e2[0] * n[1] - e2[1] * n[0]) * invDet,
  };
  const float row1[3] = {
    (n[1] * e1[2] - n[2] * e1[1]) * invDet,
    (n[2] * e1[0] - n[0] * e1[2]) * invDet,
    (n[0] * e1[1] - n[1] * e1[0]) * invDet,
  };
  const float row2[3] = {
    (e1[1] * e2[2] - e1[2] * e2[1]) * invDet,
    (e1[2] * e2[0] - e1[0] * e2[2]) * invDet,
    (e1[0] * e2[1] - e1[1] * e2[0]) * invDet,
  };

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outTransform.row0[axis] = row0[axis];
    outTransform.row1[axis] = row1[axis];
    outTransform.row2[axis] = row2[axis];
  }
  outTransform.row0[3] = -(row0[0] * v0.position[0] + row0[1] * v0.position[1] + row0[2] * v0.position[2]);
  outTransform.row1[3] = -(row1[0] * v0.position[0] + row1[1] * v0.position[1] + row1[2] * v0.position[2]);
  outTransform.row2[3] = -(row2[0] * v0.position[0] + row2[1] * v0.position[1] + row2[2] * v0.position[2]);
  return true;
}

class BuilderState
{
public:
  explicit BuilderState(const CWBVHBuilder::BuildOptions &options) : options_(options)
  {
    if (options_.sahBinCount < 2u)
    {
      throw std::runtime_error("CWBVHBuilder requires at least 2 SAH bins");
    }
  }

  CWBVHBuilder::BuildResult build(const virtualgeometry::FallbackMeshData &mesh)
  {
    CWBVHBuilder::BuildResult result{};
    result.localBounds = buildLocalBounds(mesh);
    result.primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
    result.primitiveRemap.reserve(result.primitiveCount);
    result.primitiveDobbRotationIndices.resize(result.primitiveCount, 0u);
    result.primitiveDobbProxies.resize(result.primitiveCount);
    result.triangleTransforms.resize(result.primitiveCount);
    for (uint32_t primitiveIndex = 0u; primitiveIndex < result.primitiveCount; ++primitiveIndex)
    {
      result.primitiveRemap.push_back(primitiveIndex);
      result.primitiveDobbRotationIndices[primitiveIndex] = computeTriangleDobbRotationIndex(mesh, primitiveIndex);
      result.primitiveDobbProxies[primitiveIndex] = computeTriangleDobbProjectionProxy(mesh, primitiveIndex);
      buildTriangleTransform(mesh, primitiveIndex, result.triangleTransforms[primitiveIndex]);
    }

    std::vector<BuildPrimitive> primitives;
    primitives.reserve(result.primitiveCount);
    for (uint32_t primitiveIndex = 0u; primitiveIndex < result.primitiveCount; ++primitiveIndex)
    {
      const virtualgeometry::AABB primitiveBounds = computeTriangleBounds(mesh, primitiveIndex);
      if (!isValidAabb(primitiveBounds))
      {
        continue;
      }

      BuildPrimitive primitive{};
      primitive.primitiveIndex = primitiveIndex;
      primitive.dominantBoneIndex = computeDominantBoneIndex(mesh, primitiveIndex);
      primitive.bounds = primitiveBounds;
      computeCentroid(primitiveBounds, primitive.centroid);
      primitives.push_back(primitive);
    }

    if (primitives.empty())
    {
      return result;
    }

    if (options_.preferBoneClusters)
    {
      std::stable_sort(
          primitives.begin(),
          primitives.end(),
          [](const BuildPrimitive &a, const BuildPrimitive &b)
          {
            if (a.dominantBoneIndex != b.dominantBoneIndex)
            {
              return a.dominantBoneIndex < b.dominantBoneIndex;
            }
            if (a.centroid[0] != b.centroid[0])
            {
              return a.centroid[0] < b.centroid[0];
            }
            if (a.centroid[1] != b.centroid[1])
            {
              return a.centroid[1] < b.centroid[1];
            }
            return a.centroid[2] < b.centroid[2];
          });
    }

    result.canonicalNodes.reserve(primitives.size() * 2u - 1u);
    result.rootNodeIndex = buildCanonicalNodeRecursive(result.canonicalNodes, primitives, 0u, static_cast<uint32_t>(primitives.size()), CWBVHBuilder::INVALID_NODE);
    CWBVHBuilder::rebuildBVH8(result);
    return result;
  }

  CWBVHBuilder::BuildResult buildFromAabbs(const std::vector<CWBVHBuilder::PrimitiveBounds> &inputPrimitives)
  {
    CWBVHBuilder::BuildResult result{};
    result.primitiveCount = static_cast<uint32_t>(inputPrimitives.size());
    result.primitiveRemap.reserve(result.primitiveCount);

    std::vector<BuildPrimitive> primitives;
    primitives.reserve(result.primitiveCount);
    for (const CWBVHBuilder::PrimitiveBounds &input : inputPrimitives)
    {
      if (input.primitiveIndex == CWBVHBuilder::INVALID_PRIMITIVE_INDEX || !isValidAabb(input.bounds))
      {
        continue;
      }

      if (primitives.empty())
      {
        result.localBounds = input.bounds;
      }
      else
      {
        result.localBounds = unionBounds(result.localBounds, input.bounds);
      }

      result.primitiveRemap.push_back(input.primitiveIndex);
      result.primitiveDobbRotationIndices.push_back(0u);
      result.primitiveDobbProxies.push_back(computeAabbDobbProjectionProxy(input.bounds));
      BuildPrimitive primitive{};
      primitive.primitiveIndex = static_cast<uint32_t>(result.primitiveRemap.size() - 1u);
      primitive.bounds = input.bounds;
      computeCentroid(input.bounds, primitive.centroid);
      primitives.push_back(primitive);
    }

    result.primitiveCount = static_cast<uint32_t>(result.primitiveRemap.size());
    if (primitives.empty())
    {
      result.localBounds = virtualgeometry::AABB{};
      return result;
    }

    result.canonicalNodes.reserve(primitives.size() * 2u - 1u);
    result.rootNodeIndex = buildCanonicalNodeRecursive(result.canonicalNodes, primitives, 0u, static_cast<uint32_t>(primitives.size()), CWBVHBuilder::INVALID_NODE);
    CWBVHBuilder::rebuildBVH8(result);
    return result;
  }

private:
  uint32_t buildCanonicalNodeRecursive(std::vector<CWBVHBuilder::CanonicalNode> &nodes, std::vector<BuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const uint32_t parent)
  {
    if (begin >= end)
    {
      return CWBVHBuilder::INVALID_NODE;
    }

    if (end - begin == 1u)
    {
      CWBVHBuilder::CanonicalNode node{};
      node.parent = parent;
      node.bounds = primitives[begin].bounds;
      node.primitiveIndex = primitives[begin].primitiveIndex;
      node.subtreePrimitiveCount = 1u;
      node.isLeaf = true;
      const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
      nodes.push_back(node);
      return nodeIndex;
    }

    virtualgeometry::AABB centroidBounds{};
    centroidBounds.minPoint[0] = primitives[begin].centroid[0];
    centroidBounds.minPoint[1] = primitives[begin].centroid[1];
    centroidBounds.minPoint[2] = primitives[begin].centroid[2];
    centroidBounds.maxPoint[0] = primitives[begin].centroid[0];
    centroidBounds.maxPoint[1] = primitives[begin].centroid[1];
    centroidBounds.maxPoint[2] = primitives[begin].centroid[2];

    virtualgeometry::AABB primitiveBounds = primitives[begin].bounds;
    for (uint32_t index = begin + 1u; index < end; ++index)
    {
      primitiveBounds = unionBounds(primitiveBounds, primitives[index].bounds);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        centroidBounds.minPoint[axis] = std::min(centroidBounds.minPoint[axis], primitives[index].centroid[axis]);
        centroidBounds.maxPoint[axis] = std::max(centroidBounds.maxPoint[axis], primitives[index].centroid[axis]);
      }
    }

    SplitCandidate best{};
    float bestCost = primitiveBounds.GetSurfaceArea() * static_cast<float>(end - begin);

    if (options_.splitHeuristic == CWBVHBuilder::BuildOptions::SplitHeuristic::ExactSAH)
    {
      const uint32_t count = end - begin;
      std::vector<uint32_t> order(count);
      for (uint32_t index = 0u; index < count; ++index)
      {
        order[index] = begin + index;
      }

      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        std::sort(
            order.begin(),
            order.end(),
            [&](const uint32_t a, const uint32_t b)
            {
              if (primitives[a].centroid[axis] != primitives[b].centroid[axis])
              {
                return primitives[a].centroid[axis] < primitives[b].centroid[axis];
              }
              return primitives[a].primitiveIndex < primitives[b].primitiveIndex;
            });

        std::vector<virtualgeometry::AABB> leftBounds(count - 1u);
        std::vector<virtualgeometry::AABB> rightBounds(count - 1u);

        virtualgeometry::AABB runningLeftBounds = primitives[order[0u]].bounds;
        for (uint32_t split = 0u; split + 1u < count; ++split)
        {
          if (split != 0u)
          {
            runningLeftBounds = unionBounds(runningLeftBounds, primitives[order[split]].bounds);
          }
          leftBounds[split] = runningLeftBounds;
        }

        virtualgeometry::AABB runningRightBounds = primitives[order[count - 1u]].bounds;
        for (uint32_t split = count - 1u; split-- > 0u;)
        {
          rightBounds[split] = runningRightBounds;
          runningRightBounds = unionBounds(primitives[order[split]].bounds, runningRightBounds);
        }
        for (uint32_t split = 0u; split + 1u < count; ++split)
        {
          const uint32_t leftCount = split + 1u;
          const uint32_t rightCount = count - leftCount;
          const float cost = leftBounds[split].GetSurfaceArea() * static_cast<float>(leftCount) + rightBounds[split].GetSurfaceArea() * static_cast<float>(rightCount);
          if (cost < bestCost)
          {
            bestCost = cost;
            best.axis = axis;
            best.mid = begin + leftCount;
            best.cost = cost;
          }
        }
      }
    }
    else
    {
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const float minCentroid = centroidBounds.minPoint[axis];
        const float maxCentroid = centroidBounds.maxPoint[axis];
        const float extent = maxCentroid - minCentroid;
        if (extent <= 1e-6f)
        {
          continue;
        }

        std::vector<SAHBin> bins(options_.sahBinCount);
        for (uint32_t index = begin; index < end; ++index)
        {
          const float normalized = (primitives[index].centroid[axis] - minCentroid) / extent;
          const uint32_t binIndex = std::min<uint32_t>(options_.sahBinCount - 1u, static_cast<uint32_t>(normalized * static_cast<float>(options_.sahBinCount)));
          SAHBin &bin = bins[binIndex];
          bin.bounds = bin.hasBounds ? unionBounds(bin.bounds, primitives[index].bounds) : primitives[index].bounds;
          bin.count += 1u;
          bin.hasBounds = true;
        }

        std::vector<virtualgeometry::AABB> leftBounds(options_.sahBinCount - 1u);
        std::vector<virtualgeometry::AABB> rightBounds(options_.sahBinCount - 1u);
        std::vector<uint32_t> leftCounts(options_.sahBinCount - 1u, 0u);
        std::vector<uint32_t> rightCounts(options_.sahBinCount - 1u, 0u);

        virtualgeometry::AABB runningLeftBounds{};
        uint32_t runningLeftCount = 0u;
        bool hasRunningLeftBounds = false;
        for (uint32_t split = 0u; split < options_.sahBinCount - 1u; ++split)
        {
          if (bins[split].hasBounds)
          {
            runningLeftBounds = hasRunningLeftBounds ? unionBounds(runningLeftBounds, bins[split].bounds) : bins[split].bounds;
            hasRunningLeftBounds = true;
          }
          runningLeftCount += bins[split].count;
          leftBounds[split] = runningLeftBounds;
          leftCounts[split] = runningLeftCount;
        }

        virtualgeometry::AABB runningRightBounds{};
        uint32_t runningRightCount = 0u;
        bool hasRunningRightBounds = false;
        for (uint32_t split = options_.sahBinCount - 1u; split-- > 1u;)
        {
          if (bins[split].hasBounds)
          {
            runningRightBounds = hasRunningRightBounds ? unionBounds(runningRightBounds, bins[split].bounds) : bins[split].bounds;
            hasRunningRightBounds = true;
          }
          runningRightCount += bins[split].count;
          rightBounds[split - 1u] = runningRightBounds;
          rightCounts[split - 1u] = runningRightCount;
        }

        if (bins[1].hasBounds)
        {
          runningRightBounds = hasRunningRightBounds ? unionBounds(runningRightBounds, bins[1].bounds) : bins[1].bounds;
        }
        runningRightCount += bins[1].count;
        rightBounds[0] = runningRightBounds;
        rightCounts[0] = runningRightCount;

        for (uint32_t split = 0u; split < options_.sahBinCount - 1u; ++split)
        {
          if (leftCounts[split] == 0u || rightCounts[split] == 0u)
          {
            continue;
          }

          const float cost = leftBounds[split].GetSurfaceArea() * static_cast<float>(leftCounts[split]) + rightBounds[split].GetSurfaceArea() * static_cast<float>(rightCounts[split]);
          if (cost < bestCost)
          {
            bestCost = cost;
            best.axis = axis;
            best.binSplit = split;
            best.cost = cost;
          }
        }
      }
    }

    uint32_t mid = begin + ((end - begin) >> 1u);
    if (best.axis != CWBVHBuilder::INVALID_NODE)
    {
      if (options_.splitHeuristic == CWBVHBuilder::BuildOptions::SplitHeuristic::ExactSAH)
      {
        mid = best.mid;
        std::nth_element(
            primitives.begin() + begin,
            primitives.begin() + mid,
            primitives.begin() + end,
            [axis = best.axis](const BuildPrimitive &a, const BuildPrimitive &b)
            {
              if (a.centroid[axis] != b.centroid[axis])
              {
                return a.centroid[axis] < b.centroid[axis];
              }
              return a.primitiveIndex < b.primitiveIndex;
            });
      }
      else
      {
        const float centroidMin = centroidBounds.minPoint[best.axis];
        const float centroidMax = centroidBounds.maxPoint[best.axis];
        const float extent = centroidMax - centroidMin;
        auto midIt = std::partition(
            primitives.begin() + begin,
            primitives.begin() + end,
            [&](const BuildPrimitive &primitive)
            {
              const float normalized = extent <= 1e-6f ? 0.0f : (primitive.centroid[best.axis] - centroidMin) / extent;
              const uint32_t binIndex = std::min<uint32_t>(options_.sahBinCount - 1u, static_cast<uint32_t>(normalized * static_cast<float>(options_.sahBinCount)));
              return binIndex <= best.binSplit;
            });
        mid = static_cast<uint32_t>(midIt - primitives.begin());
      }
    }

    if (options_.preferBoneClusters && primitives[begin].dominantBoneIndex != primitives[end - 1u].dominantBoneIndex)
    {
      const uint32_t targetMid = begin + ((end - begin) >> 1u);
      uint32_t bestBoundary = begin + 1u;
      uint32_t bestDistance = end - begin;
      for (uint32_t index = begin + 1u; index < end; ++index)
      {
        if (primitives[index - 1u].dominantBoneIndex == primitives[index].dominantBoneIndex)
        {
          continue;
        }

        const uint32_t distance = index > targetMid ? index - targetMid : targetMid - index;
        if (distance < bestDistance)
        {
          bestDistance = distance;
          bestBoundary = index;
        }
      }

      if (bestBoundary > begin && bestBoundary < end)
      {
        mid = bestBoundary;
      }
    }

    if (mid <= begin || mid >= end)
    {
      uint32_t splitAxis = 0u;
      float maxExtent = centroidBounds.maxPoint[0] - centroidBounds.minPoint[0];
      for (uint32_t axis = 1u; axis < 3u; ++axis)
      {
        const float extent = centroidBounds.maxPoint[axis] - centroidBounds.minPoint[axis];
        if (extent > maxExtent)
        {
          maxExtent = extent;
          splitAxis = axis;
        }
      }

      std::nth_element(
          primitives.begin() + begin,
          primitives.begin() + mid,
          primitives.begin() + end,
          [splitAxis](const BuildPrimitive &a, const BuildPrimitive &b)
          {
            return a.centroid[splitAxis] < b.centroid[splitAxis];
          });
    }

    const uint32_t nodeIndex = static_cast<uint32_t>(nodes.size());
    nodes.push_back(CWBVHBuilder::CanonicalNode{});
    nodes[nodeIndex].parent = parent;

    const uint32_t left = buildCanonicalNodeRecursive(nodes, primitives, begin, mid, nodeIndex);
    const uint32_t right = buildCanonicalNodeRecursive(nodes, primitives, mid, end, nodeIndex);

    CWBVHBuilder::CanonicalNode &node = nodes[nodeIndex];
    node.left = left;
    node.right = right;
    if (left != CWBVHBuilder::INVALID_NODE)
    {
      node.bounds = nodes[left].bounds;
      node.subtreePrimitiveCount += nodes[left].subtreePrimitiveCount;
    }
    if (right != CWBVHBuilder::INVALID_NODE)
    {
      node.bounds = left == CWBVHBuilder::INVALID_NODE ? nodes[right].bounds : unionBounds(node.bounds, nodes[right].bounds);
      node.subtreePrimitiveCount += nodes[right].subtreePrimitiveCount;
    }

    return nodeIndex;
  }

  CWBVHBuilder::BuildOptions options_{};
};

} // namespace

CWBVHBuilder::BuildResult CWBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh)
{
  return build(mesh, BuildOptions{});
}

CWBVHBuilder::BuildResult CWBVHBuilder::build(const virtualgeometry::FallbackMeshData &mesh, const BuildOptions &options)
{
  return BuilderState(options).build(mesh);
}

CWBVHBuilder::BuildResult CWBVHBuilder::buildOffline(const virtualgeometry::FallbackMeshData &mesh)
{
  BuildOptions options{};
  if (meshUsesSkinning(mesh))
  {
    options.sahBinCount = 16u;
    options.preferBoneClusters = true;
    options.splitHeuristic = BuildOptions::SplitHeuristic::BinnedSAH;
  }
  else
  {
    options.sahBinCount = 64u;
    options.preferBoneClusters = false;
    options.splitHeuristic = BuildOptions::SplitHeuristic::ExactSAH;
  }
  return build(mesh, options);
}

CWBVHBuilder::BuildResult CWBVHBuilder::buildFromAabbs(const std::vector<PrimitiveBounds> &primitives)
{
  return buildFromAabbs(primitives, BuildOptions{});
}

CWBVHBuilder::BuildResult CWBVHBuilder::buildFromAabbs(const std::vector<PrimitiveBounds> &primitives, const BuildOptions &options)
{
  CWBVHBuilder::BuildOptions adjustedOptions = options;
  adjustedOptions.preferBoneClusters = false;
  return BuilderState(adjustedOptions).buildFromAabbs(primitives);
}

void CWBVHBuilder::rebuildBVH8(BuildResult &build)
{
  build.bvh8Nodes.clear();
  build.bvh8LeafRemap.clear();
  if (build.rootNodeIndex == INVALID_NODE || build.rootNodeIndex >= build.canonicalNodes.size())
  {
    return;
  }

  build.bvh8Nodes.push_back(BVH8Node{});
  emitBVH8Recursive(build.canonicalNodes, build.rootNodeIndex, 0u, build.bvh8Nodes, build.bvh8LeafRemap);
}

CWBVHBuilder::CompressionResult CWBVHBuilder::compressBVH8(const std::vector<BVH8Node> &bvh8Nodes, const std::vector<uint32_t> &leafRemap)
{
  return compressBVH8(bvh8Nodes, leafRemap, CompressionOptions{});
}

CWBVHBuilder::CompressionResult CWBVHBuilder::compressBVH8(const std::vector<BVH8Node> &bvh8Nodes, const std::vector<uint32_t> &leafRemap, const CompressionOptions &options)
{
  CompressionResult result{};
  result.leafRemap = leafRemap;
  if (bvh8Nodes.empty())
  {
    return result;
  }

  result.nodes.resize(bvh8Nodes.size());
  if (options.buildDobbNodes)
  {
    result.dobbNodes.resize(bvh8Nodes.size());
    result.nodeMetadata.resize(bvh8Nodes.size());
  }
  std::vector<uint32_t> outputToSourceNode(bvh8Nodes.size(), INVALID_NODE);
  std::vector<uint32_t> nodeDepths(bvh8Nodes.size(), UINT32_MAX);
  for (uint32_t sourceIndex = 0u; sourceIndex < static_cast<uint32_t>(bvh8Nodes.size()); ++sourceIndex)
  {
    const uint32_t outputIndex = bvh8Nodes[sourceIndex].cwbvhOutputIndex;
    if (outputIndex != INVALID_NODE && outputIndex < outputToSourceNode.size())
    {
      outputToSourceNode[outputIndex] = sourceIndex;
    }
  }
  if (!nodeDepths.empty())
  {
    nodeDepths[0u] = 0u;
    for (uint32_t outputIndex = 0u; outputIndex < static_cast<uint32_t>(outputToSourceNode.size()); ++outputIndex)
    {
      const uint32_t sourceIndex = outputToSourceNode[outputIndex];
      if (sourceIndex == INVALID_NODE || nodeDepths[outputIndex] == UINT32_MAX)
      {
        continue;
      }
      const BVH8Node &sourceNode = bvh8Nodes[sourceIndex];
      uint32_t internalChildOffset = 0u;
      for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
      {
        if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) == 0u ||
            (sourceNode.childMeta[childSlot] & BVH8_CHILD_LEAF) != 0u)
        {
          continue;
        }
        const uint32_t childOutputIndex = sourceNode.internalChildBase + internalChildOffset;
        if (sourceNode.internalChildBase != INVALID_NODE && childOutputIndex < nodeDepths.size())
        {
          nodeDepths[childOutputIndex] = std::min(nodeDepths[childOutputIndex], nodeDepths[outputIndex] + 1u);
        }
        ++internalChildOffset;
      }
    }
  }
  std::vector<uint32_t> nodeCandidateRotationIndices(bvh8Nodes.size(), 0u);
  std::vector<DobbProjectionProxy> nodeDobbProxies(bvh8Nodes.size());
  for (DobbProjectionProxy &proxy : nodeDobbProxies)
  {
    proxy = emptyDobbProjectionProxy();
  }
  for (uint32_t reverseOutputIndex = 0u; reverseOutputIndex < static_cast<uint32_t>(outputToSourceNode.size()); ++reverseOutputIndex)
  {
    const uint32_t outputIndex = static_cast<uint32_t>(outputToSourceNode.size()) - 1u - reverseOutputIndex;
    const uint32_t sourceIndex = outputToSourceNode[outputIndex];
    if (sourceIndex == INVALID_NODE)
    {
      continue;
    }

    const BVH8Node &sourceNode = bvh8Nodes[sourceIndex];
    float largestChildArea = -1.0f;
    uint32_t largestChildRotationIndex = 0u;
    DobbProjectionProxy nodeProxy = emptyDobbProjectionProxy();
    uint32_t internalChildOffset = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) == 0u)
      {
        continue;
      }

      uint32_t childRotationIndex = 0u;
      DobbProjectionProxy childProxy = emptyDobbProjectionProxy();
      if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_LEAF) == 0u)
      {
        const uint32_t childOutputIndex = sourceNode.internalChildBase + internalChildOffset;
        if (sourceNode.internalChildBase != INVALID_NODE && childOutputIndex < nodeCandidateRotationIndices.size())
        {
          childRotationIndex = nodeCandidateRotationIndices[childOutputIndex];
          childProxy = nodeDobbProxies[childOutputIndex];
        }
        ++internalChildOffset;
      }
      else if (sourceNode.childIndex[childSlot] < result.leafRemap.size())
      {
        const uint32_t localPrimitiveIndex = result.leafRemap[sourceNode.childIndex[childSlot]];
        if (options.primitiveDobbRotationIndices != nullptr && localPrimitiveIndex < options.primitiveDobbRotationIndices->size())
        {
          childRotationIndex = (*options.primitiveDobbRotationIndices)[localPrimitiveIndex];
        }
        if (options.primitiveDobbProxies != nullptr && localPrimitiveIndex < options.primitiveDobbProxies->size())
        {
          childProxy = (*options.primitiveDobbProxies)[localPrimitiveIndex];
        }
      }

      if (childProxy.valid == 0u)
      {
        virtualgeometry::AABB fallbackBounds{};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          fallbackBounds.minPoint[axis] = sourceNode.childMin[childSlot][axis];
          fallbackBounds.maxPoint[axis] = sourceNode.childMax[childSlot][axis];
        }
        childProxy = computeAabbDobbProjectionProxy(fallbackBounds);
      }
      nodeProxy = mergeDobbProjectionProxy(nodeProxy, childProxy);

      const float childArea = aabbSurfaceArea(sourceNode.childMin[childSlot], sourceNode.childMax[childSlot]);
      if (childRotationIndex != 0u && childArea > largestChildArea)
      {
        largestChildArea = childArea;
        largestChildRotationIndex = childRotationIndex;
      }
    }
    nodeCandidateRotationIndices[outputIndex] = largestChildRotationIndex;
    nodeDobbProxies[outputIndex] = nodeProxy;
  }
  result.stats.nodeCount = static_cast<uint32_t>(bvh8Nodes.size());
  for (const BVH8Node &sourceNode : bvh8Nodes)
  {
    if (sourceNode.cwbvhOutputIndex == INVALID_NODE || sourceNode.cwbvhOutputIndex >= result.nodes.size())
    {
      throw std::runtime_error("CWBVHBuilder BVH8 node has invalid CWBVH output index");
    }

    auto packNode = [&](const virtualgeometry::AABB &parentBounds,
                        const std::array<virtualgeometry::AABB, 8u> &childBounds,
                        CompressionStats *stats) -> PackedCWBVHNode
    {
      const float parentMinX = parentBounds.minPoint[0];
      const float parentMinY = parentBounds.minPoint[1];
      const float parentMinZ = parentBounds.minPoint[2];
      const uint32_t sxExp = encodeSignedExponent8(parentBounds.maxPoint[0] - parentBounds.minPoint[0]);
      const uint32_t syExp = encodeSignedExponent8(parentBounds.maxPoint[1] - parentBounds.minPoint[1]);
      const uint32_t szExp = encodeSignedExponent8(parentBounds.maxPoint[2] - parentBounds.minPoint[2]);
      const float scaleX = decodeExponentScale(sxExp);
      const float scaleY = decodeExponentScale(syExp);
      const float scaleZ = decodeExponentScale(szExp);

      PackedCWBVHNode node{};
      uint32_t imask = 0u;
      for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
      {
        if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) == 0u)
        {
          continue;
        }

        const virtualgeometry::AABB &bounds = childBounds[childSlot];
        const uint32_t arrayIndex = childSlot >> 2u;
        const uint32_t bitOffset = (childSlot & 3u) * 8u;
        const uint32_t qlo[3] = {
          quantizeLowerBound(bounds.minPoint[0], parentMinX, scaleX),
          quantizeLowerBound(bounds.minPoint[1], parentMinY, scaleY),
          quantizeLowerBound(bounds.minPoint[2], parentMinZ, scaleZ),
        };
        const uint32_t qhi[3] = {
          quantizeUpperBound(bounds.maxPoint[0], parentMinX, scaleX),
          quantizeUpperBound(bounds.maxPoint[1], parentMinY, scaleY),
          quantizeUpperBound(bounds.maxPoint[2], parentMinZ, scaleZ),
        };
        node.qlox[arrayIndex] |= qlo[0] << bitOffset;
        node.qloy[arrayIndex] |= qlo[1] << bitOffset;
        node.qloz[arrayIndex] |= qlo[2] << bitOffset;
        node.qhix[arrayIndex] |= qhi[0] << bitOffset;
        node.qhiy[arrayIndex] |= qhi[1] << bitOffset;
        node.qhiz[arrayIndex] |= qhi[2] << bitOffset;

        if (stats != nullptr)
        {
          ++stats->validChildCount;
          for (uint32_t axis = 0u; axis < 3u; ++axis)
          {
            stats->saturatedLowerBounds += qlo[axis] == 0u ? 1u : 0u;
            stats->saturatedUpperBounds += qhi[axis] == 255u ? 1u : 0u;
          }

          const float parentMin[3] = {parentMinX, parentMinY, parentMinZ};
          const float scale[3] = {scaleX, scaleY, scaleZ};
          float decodedMin[3]{};
          float decodedMax[3]{};
          bool conservative = true;
          float maxAxisExpansion = 0.0f;
          for (uint32_t axis = 0u; axis < 3u; ++axis)
          {
            decodedMin[axis] = parentMin[axis] + static_cast<float>(qlo[axis]) * scale[axis];
            decodedMax[axis] = parentMin[axis] + static_cast<float>(qhi[axis]) * scale[axis];
            const float sourceExtent = std::max(1.0f, bounds.maxPoint[axis] - bounds.minPoint[axis]);
            const float epsilon = sourceExtent * 1.0e-5f;
            if (decodedMin[axis] > bounds.minPoint[axis] + epsilon ||
                decodedMax[axis] < bounds.maxPoint[axis] - epsilon)
            {
              conservative = false;
            }
            maxAxisExpansion = std::max(
                maxAxisExpansion,
                std::max(0.0f, bounds.minPoint[axis] - decodedMin[axis]) +
                    std::max(0.0f, decodedMax[axis] - bounds.maxPoint[axis]));
          }

          const float sourceArea = aabbSurfaceArea(bounds);
          const float decodedArea = aabbSurfaceArea(decodedMin, decodedMax);
          if (maxAxisExpansion > 0.0f || decodedArea > sourceArea)
          {
            ++stats->expandedChildCount;
          }
          stats->sourceSurfaceAreaSum += static_cast<double>(sourceArea);
          stats->decodedSurfaceAreaSum += static_cast<double>(decodedArea);
          stats->maxAxisExpansion = std::max(stats->maxAxisExpansion, maxAxisExpansion);
          stats->maxSurfaceAreaExpansion = std::max(stats->maxSurfaceAreaExpansion, decodedArea / std::max(sourceArea, 1.0e-6f));

          if (conservative)
          {
            ++stats->conservativeChildCount;
          }
          else
          {
            ++stats->nonConservativeChildCount;
            throw std::runtime_error("CWBVHBuilder quantized child bounds are not conservative");
          }
        }

        if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_LEAF) == 0u)
        {
          if (stats != nullptr)
          {
            ++stats->internalChildCount;
          }
          imask |= 1u << childSlot;
          node.info[arrayIndex] |= ((1u << 5u) | ((childSlot + 24u) & 0x1fu)) << bitOffset;
        }
        else
        {
          if (stats != nullptr)
          {
            ++stats->leafChildCount;
          }
          if (sourceNode.childIndex[childSlot] < sourceNode.leafBase)
          {
            throw std::runtime_error("CWBVHBuilder BVH8 leaf child index precedes leaf base");
          }
          const uint32_t leafOffset = sourceNode.childIndex[childSlot] - sourceNode.leafBase;
          if (leafOffset >= 24u)
          {
            throw std::runtime_error("CWBVHBuilder BVH8 leaf offset exceeds CWBVH leaf encoding range");
          }
          if (sourceNode.childIndex[childSlot] >= result.leafRemap.size())
          {
            throw std::runtime_error("CWBVHBuilder BVH8 leaf child index exceeds leaf remap");
          }
          node.info[arrayIndex] |= ((1u << 5u) | (leafOffset & 0x1fu)) << bitOffset;
        }
      }

      node.orxy[0] = bitCastU32(parentMinX);
      node.orxy[1] = bitCastU32(parentMinY);
      node.oeim[0] = bitCastU32(parentMinZ);
      node.oeim[1] = pack4xU8(sxExp, syExp, szExp, imask);
      node.ntbi[0] = sourceNode.internalChildBase == INVALID_NODE ? 0u : sourceNode.internalChildBase;
      node.ntbi[1] = sourceNode.leafBase;
      return node;
    };

    std::array<virtualgeometry::AABB, 8u> childBounds{};
    std::array<DobbProjectionProxy, 8u> childDobbProxies{};
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      childBounds[childSlot].minPoint[0] = sourceNode.childMin[childSlot][0];
      childBounds[childSlot].minPoint[1] = sourceNode.childMin[childSlot][1];
      childBounds[childSlot].minPoint[2] = sourceNode.childMin[childSlot][2];
      childBounds[childSlot].maxPoint[0] = sourceNode.childMax[childSlot][0];
      childBounds[childSlot].maxPoint[1] = sourceNode.childMax[childSlot][1];
      childBounds[childSlot].maxPoint[2] = sourceNode.childMax[childSlot][2];
      childDobbProxies[childSlot] = emptyDobbProjectionProxy();
    }

    uint32_t internalChildOffsetForProxy = 0u;
    for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
    {
      if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) == 0u)
      {
        continue;
      }

      if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_LEAF) == 0u)
      {
        const uint32_t childOutputIndex = sourceNode.internalChildBase + internalChildOffsetForProxy;
        if (sourceNode.internalChildBase != INVALID_NODE && childOutputIndex < nodeDobbProxies.size())
        {
          childDobbProxies[childSlot] = nodeDobbProxies[childOutputIndex];
        }
        ++internalChildOffsetForProxy;
      }
      else if (sourceNode.childIndex[childSlot] < result.leafRemap.size())
      {
        const uint32_t localPrimitiveIndex = result.leafRemap[sourceNode.childIndex[childSlot]];
        if (options.primitiveDobbProxies != nullptr && localPrimitiveIndex < options.primitiveDobbProxies->size())
        {
          childDobbProxies[childSlot] = (*options.primitiveDobbProxies)[localPrimitiveIndex];
        }
      }
      if (childDobbProxies[childSlot].valid == 0u)
      {
        childDobbProxies[childSlot] = computeAabbDobbProjectionProxy(childBounds[childSlot]);
      }
    }

    result.nodes[sourceNode.cwbvhOutputIndex] = packNode(sourceNode.bounds, childBounds, &result.stats);

    if (options.buildDobbNodes)
    {
      CWBVHNodeMetadata metadata{};
      metadata.flags = kDobbNodeFlagQuantized;
      metadata.quantizedFlags = 1u;
      virtualgeometry::AABB dobbParentBounds = sourceNode.bounds;
      std::array<virtualgeometry::AABB, 8u> dobbChildBounds = childBounds;
      uint32_t rotationIndex = 0u;

      const uint32_t nodeDepth = sourceNode.cwbvhOutputIndex < nodeDepths.size() ? nodeDepths[sourceNode.cwbvhOutputIndex] : UINT32_MAX;
      const uint32_t candidateRotationIndex = sourceNode.cwbvhOutputIndex < nodeCandidateRotationIndices.size() ? nodeCandidateRotationIndices[sourceNode.cwbvhOutputIndex] : 0u;
      if (nodeDepth <= options.dobbMaxConvertedLevels &&
          candidateRotationIndex != 0u &&
          options.dobbRotationSet == kDobbFixed104RotationSet)
      {
        float baselineCost = 0.0f;
        for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
        {
          if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) != 0u)
          {
            baselineCost += aabbSurfaceArea(childBounds[childSlot]);
          }
        }

        float bestCost = baselineCost;
        uint32_t bestRotationIndex = 0u;
        virtualgeometry::AABB bestParentBounds = sourceNode.bounds;
        std::array<virtualgeometry::AABB, 8u> bestChildBounds = childBounds;
        if ((candidateRotationIndex % kDobbAngleCount) != 0u)
        {
          virtualgeometry::AABB rotatedParentBounds = emptyAabb();
          std::array<virtualgeometry::AABB, 8u> rotatedChildBounds{};
          float rotatedCost = 0.0f;
          bool hasChild = false;
          for (uint32_t childSlot = 0u; childSlot < 8u; ++childSlot)
          {
            if ((sourceNode.childMeta[childSlot] & BVH8_CHILD_VALID) == 0u)
            {
              continue;
            }
            rotatedChildBounds[childSlot] = refitDobbProjectionProxyInRotatedSpace(childDobbProxies[childSlot], candidateRotationIndex);
            rotatedParentBounds = hasChild ? unionBoundsLocal(rotatedParentBounds, rotatedChildBounds[childSlot]) : rotatedChildBounds[childSlot];
            rotatedCost += aabbSurfaceArea(rotatedChildBounds[childSlot]);
            hasChild = true;
          }

          if (hasChild && rotatedCost < bestCost)
          {
            bestCost = rotatedCost;
            bestRotationIndex = candidateRotationIndex;
            bestParentBounds = rotatedParentBounds;
            bestChildBounds = rotatedChildBounds;
          }
        }

        const float acceptanceAlpha = std::clamp(options.dobbAcceptanceAlpha, 0.0f, 1.0f);
        if (bestRotationIndex != 0u && baselineCost > 0.0f && bestCost <= baselineCost * acceptanceAlpha)
        {
          rotationIndex = bestRotationIndex;
          dobbParentBounds = bestParentBounds;
          dobbChildBounds = bestChildBounds;
          metadata.flags |= kDobbNodeFlagOriented;
        }
      }

      metadata.rotationIndex = rotationIndex;
      result.dobbNodes[sourceNode.cwbvhOutputIndex] = packNode(dobbParentBounds, dobbChildBounds, nullptr);
      result.nodeMetadata[sourceNode.cwbvhOutputIndex] = metadata;
    }
  }
  return result;
}

} // namespace softwarebvh
