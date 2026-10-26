#include "softwarebvh/SoftwareTLASManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace softwarebvh
{

namespace
{

static uint32_t divCeilU32(uint32_t value, uint32_t divisor)
{
  return divisor == 0u ? 0u : (value + divisor - 1u) / divisor;
}

constexpr uint32_t kRadixSortWorkgroupSize = 256u;
constexpr uint32_t kRadixSortElementsPerThread = 4u;
constexpr uint32_t kRadixSortBlockSize = kRadixSortWorkgroupSize * kRadixSortElementsPerThread;
constexpr uint32_t kRadixSortBinCount = 16u;
constexpr uint32_t kRadixSortDispatchSlotCount = 6u;

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

static SoftwareTLASManager::InstanceGPU makeInstanceGPU(const SoftwareTLASManager::InstanceBuildInput &input)
{
  SoftwareTLASManager::InstanceGPU instanceGpu{};
  const math::Mat4f inverseTransform = input.transform.inverse();
  std::memcpy(instanceGpu.transform, input.transform.data, sizeof(instanceGpu.transform));
  std::memcpy(instanceGpu.inverseTransform, inverseTransform.data, sizeof(instanceGpu.inverseTransform));
  std::memcpy(instanceGpu.previousTransform, input.previousTransform.data, sizeof(instanceGpu.previousTransform));
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    instanceGpu.aabbMin[axis] = input.worldBounds.minPoint[axis];
    instanceGpu.aabbMax[axis] = input.worldBounds.maxPoint[axis];
  }
  instanceGpu.blasHandle = input.blasHandle;
  instanceGpu.sceneInstanceId = input.instanceId;
  instanceGpu.flags = input.flags;
  return instanceGpu;
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

static constexpr uint32_t kTlasDobbNodeFlagOriented = 1u << 0u;
static constexpr uint32_t kTlasDobbNodeFlagQuantized = 1u << 1u;
static constexpr uint32_t kTlasDobbFixed104RotationSet = 0u;
static constexpr uint32_t kTlasDobbAxisCount = 13u;
static constexpr uint32_t kTlasDobbAngleCount = 8u;
static constexpr uint32_t kTlasDobbRotationCount = kTlasDobbAxisCount * kTlasDobbAngleCount;
static constexpr uint32_t kTlasDobbPlaneCount = kTlasDobbAxisCount * 2u;
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
  static constexpr Vec3 kAxes[kTlasDobbAxisCount] = {
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
  return normalizeVec3(kAxes[axisIndex % kTlasDobbAxisCount]);
}

static Vec3 dobbPlaneAxis(const uint32_t planeIndex)
{
  const Vec3 axis = dobbAxis(planeIndex % kTlasDobbAxisCount);
  return planeIndex < kTlasDobbAxisCount ? axis : mulVec3(axis, -1.0f);
}

static Vec3 rotateDobbVector(const Vec3 value, const uint32_t rotationIndex)
{
  if (rotationIndex == 0u)
  {
    return value;
  }

  const uint32_t axisIndex = (rotationIndex / kTlasDobbAngleCount) % kTlasDobbAxisCount;
  const uint32_t angleIndex = rotationIndex % kTlasDobbAngleCount;
  if (angleIndex == 0u)
  {
    return value;
  }

  const Vec3 axis = dobbAxis(axisIndex);
  const float angle = static_cast<float>(angleIndex) * (kPi / static_cast<float>(kTlasDobbAngleCount));
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

  const uint32_t axisIndex = (rotationIndex / kTlasDobbAngleCount) % kTlasDobbAxisCount;
  const uint32_t angleIndex = rotationIndex % kTlasDobbAngleCount;
  if (angleIndex == 0u)
  {
    return value;
  }

  const Vec3 axis = dobbAxis(axisIndex);
  const float angle = -static_cast<float>(angleIndex) * (kPi / static_cast<float>(kTlasDobbAngleCount));
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

struct DobbApexFacet
{
  uint32_t planeIndex[3]{};
  Vec3 normal{};
  float offset = 0.0f;
};

static float signedPlaneDistanceForProxy(const SoftwareTLASManager::DobbProjectionProxy &proxy, const uint32_t planeIndex)
{
  const uint32_t axisIndex = planeIndex % kTlasDobbAxisCount;
  return planeIndex < kTlasDobbAxisCount ? proxy.maxProjection[axisIndex] : -proxy.minProjection[axisIndex];
}

static SoftwareTLASManager::DobbProjectionProxy emptyDobbProjectionProxy()
{
  SoftwareTLASManager::DobbProjectionProxy proxy{};
  proxy.valid = 0u;
  for (uint32_t axisIndex = 0u; axisIndex < kTlasDobbAxisCount; ++axisIndex)
  {
    proxy.minProjection[axisIndex] = std::numeric_limits<float>::infinity();
    proxy.maxProjection[axisIndex] = -std::numeric_limits<float>::infinity();
  }
  return proxy;
}

static void includePointInDobbProjectionProxy(SoftwareTLASManager::DobbProjectionProxy &proxy, const Vec3 point)
{
  proxy.valid = 1u;
  for (uint32_t axisIndex = 0u; axisIndex < kTlasDobbAxisCount; ++axisIndex)
  {
    const float projection = dotVec3(dobbAxis(axisIndex), point);
    proxy.minProjection[axisIndex] = std::min(proxy.minProjection[axisIndex], projection);
    proxy.maxProjection[axisIndex] = std::max(proxy.maxProjection[axisIndex], projection);
  }
}

static SoftwareTLASManager::DobbProjectionProxy mergeDobbProjectionProxy(const SoftwareTLASManager::DobbProjectionProxy &a, const SoftwareTLASManager::DobbProjectionProxy &b)
{
  if (a.valid == 0u)
  {
    return b;
  }
  if (b.valid == 0u)
  {
    return a;
  }

  SoftwareTLASManager::DobbProjectionProxy result{};
  result.valid = 1u;
  for (uint32_t axisIndex = 0u; axisIndex < kTlasDobbAxisCount; ++axisIndex)
  {
    result.minProjection[axisIndex] = std::min(a.minProjection[axisIndex], b.minProjection[axisIndex]);
    result.maxProjection[axisIndex] = std::max(a.maxProjection[axisIndex], b.maxProjection[axisIndex]);
  }
  return result;
}

static SoftwareTLASManager::DobbProjectionProxy computeAabbDobbProjectionProxy(const virtualgeometry::AABB &bounds)
{
  SoftwareTLASManager::DobbProjectionProxy proxy = emptyDobbProjectionProxy();
  if (bounds.minPoint[0] > bounds.maxPoint[0] ||
      bounds.minPoint[1] > bounds.maxPoint[1] ||
      bounds.minPoint[2] > bounds.maxPoint[2])
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

static bool solvePlaneIntersection(const Vec3 n0, const float d0, const Vec3 n1, const float d1, const Vec3 n2, const float d2, Vec3 &outPoint)
{
  const float det = dotVec3(n0, crossVec3(n1, n2));
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
    for (uint32_t i = 0u; i < kTlasDobbPlaneCount; ++i)
    {
      const Vec3 pi = dobbPlaneAxis(i);
      for (uint32_t j = i + 1u; j < kTlasDobbPlaneCount; ++j)
      {
        const Vec3 pj = dobbPlaneAxis(j);
        for (uint32_t k = j + 1u; k < kTlasDobbPlaneCount; ++k)
        {
          const Vec3 pk = dobbPlaneAxis(k);
          Vec3 normal = crossVec3(subVec3(pj, pi), subVec3(pk, pi));
          const float normalLengthSq = dotVec3(normal, normal);
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
          for (uint32_t pointIndex = 0u; pointIndex < kTlasDobbPlaneCount; ++pointIndex)
          {
            if (dotVec3(normal, dobbPlaneAxis(pointIndex)) - offset > 1.0e-4f)
            {
              isFacet = false;
              break;
            }
          }
          if (isFacet)
          {
            result.push_back(DobbApexFacet{.planeIndex = {i, j, k}, .normal = normal, .offset = offset});
          }
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
    if (pointInTriangle(point, dobbPlaneAxis(facet.planeIndex[0u]), dobbPlaneAxis(facet.planeIndex[1u]), dobbPlaneAxis(facet.planeIndex[2u])))
    {
      return &facet;
    }
  }
  return bestFallback;
}

static float supportDobbProjectionProxyExact(const SoftwareTLASManager::DobbProjectionProxy &proxy, const Vec3 direction)
{
  float best = -std::numeric_limits<float>::infinity();
  for (uint32_t i = 0u; i < kTlasDobbPlaneCount; ++i)
  {
    const Vec3 n0 = dobbPlaneAxis(i);
    const float d0 = signedPlaneDistanceForProxy(proxy, i);
    for (uint32_t j = i + 1u; j < kTlasDobbPlaneCount; ++j)
    {
      const Vec3 n1 = dobbPlaneAxis(j);
      const float d1 = signedPlaneDistanceForProxy(proxy, j);
      for (uint32_t k = j + 1u; k < kTlasDobbPlaneCount; ++k)
      {
        const Vec3 n2 = dobbPlaneAxis(k);
        const float d2 = signedPlaneDistanceForProxy(proxy, k);
        Vec3 point{};
        if (!solvePlaneIntersection(n0, d0, n1, d1, n2, d2, point))
        {
          continue;
        }

        bool inside = true;
        for (uint32_t planeIndex = 0u; planeIndex < kTlasDobbPlaneCount; ++planeIndex)
        {
          if (dotVec3(dobbPlaneAxis(planeIndex), point) > signedPlaneDistanceForProxy(proxy, planeIndex) + 1.0e-4f)
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

static float supportDobbProjectionProxyAabbFallback(const SoftwareTLASManager::DobbProjectionProxy &proxy, const Vec3 direction)
{
  const float x = direction.x >= 0.0f ? proxy.maxProjection[0u] : proxy.minProjection[0u];
  const float y = direction.y >= 0.0f ? proxy.maxProjection[1u] : proxy.minProjection[1u];
  const float z = direction.z >= 0.0f ? proxy.maxProjection[2u] : proxy.minProjection[2u];
  return direction.x * x + direction.y * y + direction.z * z;
}

static float supportDobbProjectionProxy(const SoftwareTLASManager::DobbProjectionProxy &proxy, const Vec3 direction)
{
  if (proxy.valid == 0u)
  {
    return 0.0f;
  }

  float apexSupport = -std::numeric_limits<float>::infinity();
  const DobbApexFacet *facet = findDobbApexFacet(direction);
  if (facet != nullptr)
  {
    Vec3 apexPoint{};
    if (solvePlaneIntersection(
            dobbPlaneAxis(facet->planeIndex[0u]),
            signedPlaneDistanceForProxy(proxy, facet->planeIndex[0u]),
            dobbPlaneAxis(facet->planeIndex[1u]),
            signedPlaneDistanceForProxy(proxy, facet->planeIndex[1u]),
            dobbPlaneAxis(facet->planeIndex[2u]),
            signedPlaneDistanceForProxy(proxy, facet->planeIndex[2u]),
            apexPoint))
    {
      apexSupport = dotVec3(direction, apexPoint);
    }
  }

  if (std::isfinite(apexSupport))
  {
    return apexSupport;
  }
  return supportDobbProjectionProxyAabbFallback(proxy, direction);
}

static virtualgeometry::AABB refitDobbProjectionProxyInRotatedSpace(const SoftwareTLASManager::DobbProjectionProxy &proxy, const uint32_t rotationIndex)
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
    bounds.maxPoint[axis] = std::nextafter(supportDobbProjectionProxy(proxy, sourceAxis), std::numeric_limits<float>::infinity());
    bounds.minPoint[axis] = std::nextafter(-supportDobbProjectionProxy(proxy, mulVec3(sourceAxis, -1.0f)), -std::numeric_limits<float>::infinity());
  }
  return bounds;
}

static uint32_t closestDobbRotationForTransform(const math::Mat4f &transform)
{
  Vec3 axes[3] = {
    normalizeVec3(makeVec3(transform.data[0], transform.data[1], transform.data[2])),
    normalizeVec3(makeVec3(transform.data[4], transform.data[5], transform.data[6])),
    normalizeVec3(makeVec3(transform.data[8], transform.data[9], transform.data[10])),
  };

  float bestScore = 0.0f;
  uint32_t bestRotationIndex = 0u;
  for (uint32_t rotationIndex = 1u; rotationIndex < kTlasDobbRotationCount; ++rotationIndex)
  {
    if ((rotationIndex % kTlasDobbAngleCount) == 0u)
    {
      continue;
    }

    float score = 0.0f;
    for (const Vec3 axis : axes)
    {
      const Vec3 rotated = rotateDobbVector(axis, rotationIndex);
      score += std::max({std::fabs(rotated.x), std::fabs(rotated.y), std::fabs(rotated.z)});
    }
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

struct BVH4SAHBin
{
  virtualgeometry::AABB bounds{};
  uint32_t count = 0u;
};

struct BVH4BuildRange
{
  uint32_t begin = 0u;
  uint32_t end = 0u;
};

static uint32_t bvh4ChildMeta(const bool isLeaf)
{
  return isLeaf ? 1u : 2u;
}

} // namespace

SoftwareTLASManager::SoftwareTLASManager(rendering::RenderGraph *renderGraph) : SoftwareTLASManager(renderGraph, Settings{})
{
}

SoftwareTLASManager::SoftwareTLASManager(rendering::RenderGraph *renderGraph, Settings settings) : renderGraph_(renderGraph), settings_(settings)
{
  if (renderGraph_ == nullptr)
  {
    throw std::runtime_error("SoftwareTLASManager requires a valid RenderGraph");
  }

  if (settings_.maxInstances == 0u || settings_.workgroupSize == 0u)
  {
    throw std::runtime_error("SoftwareTLASManager settings must be non-zero");
  }

  initializeResources();
  uploadIdleState(0u);
}

SoftwareTLASManager::~SoftwareTLASManager()
{
  for (const auto &[_, bufferIds] : frameOverrideBufferIds_)
  {
    for (const rendering::BufferId bufferId : bufferIds)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph_->getRHI()->deleteBuffer(bufferId);
      }
    }
  }

  auto deleteBufferIfValid = [&](const rendering::Buffer &buffer)
  {
    if (buffer.isValid())
    {
      renderGraph_->deleteBuffer(buffer);
    }
  };

  deleteBufferIfValid(countersBuffer_);
  deleteBufferIfValid(dirtyInstanceIndicesBuffer_);
  deleteBufferIfValid(refitIndirectArgsBuffer_);
  deleteBufferIfValid(buildSortIndirectArgsBuffer_);
  deleteBufferIfValid(buildSortCountBuffer_);
  deleteBufferIfValid(buildIndirectArgsBuffer_);
  deleteBufferIfValid(refitCwbvhIndirectArgsBuffer_);
  deleteBufferIfValid(leafInstanceIndicesBuffer_);
  deleteBufferIfValid(buildDebugBuffer_);
  deleteBufferIfValid(buildSignalsBuffer_);
  deleteBufferIfValid(buildFlagsBuffer_);
  deleteBufferIfValid(buildInstanceIndicesBuffer_);
  deleteBufferIfValid(buildMortonCodesBuffer_);
  deleteBufferIfValid(cwbvhNodeRefsBuffer_);
  deleteBufferIfValid(cwbvhNodeMetadataBuffer_);
  deleteBufferIfValid(dobbCwbvhNodesBuffer_);
  deleteBufferIfValid(bvh4NodesBuffer_);
  deleteBufferIfValid(cwbvhNodesBuffer_);
  deleteBufferIfValid(buildHLBVHNodesBuffer_);
  deleteBufferIfValid(canonicalNodesBuffer_);
  deleteBufferIfValid(instanceAabbsBuffer_);
  deleteBufferIfValid(instancesBuffer_);
}

SoftwareTLASManager::BuildSummary SoftwareTLASManager::rebuild(const std::vector<InstanceBuildInput> &instances, const uint32_t dirtyInstanceCount)
{
  if (instances.size() > settings_.maxInstances)
  {
    throw std::runtime_error("SoftwareTLASManager maxInstances exceeded");
  }

  const uint32_t rootNodeIndex = rebuildGpuState(instances);
  lastBuildSummary_.instanceCount = static_cast<uint32_t>(instancesGPU_.size());
  lastBuildSummary_.nodeCount = static_cast<uint32_t>(canonicalNodesGPU_.size());
  lastBuildSummary_.rootNodeIndex = rootNodeIndex;
  lastBuildSummary_.dirtyInstanceCount = dirtyInstanceCount;
  lastBuildSummary_.buildRequested = true;
  lastBuildSummary_.refitRequested = false;
  lastBuildSummary_.rebuilt = true;
  lastBuildSummary_.refitted = false;
  lastBuildSummary_.partialRebuilt = false;
  lastBuildSummary_.partialRebuildRootCount = 0u;
  lastBuildSummary_.bvh4Rebuilt = true;
  lastBuildSummary_.bvh4Refitted = false;
  lastBuildSummary_.bvh4RebuiltForSah = false;
  ++lastBuildSummary_.buildRevision;
  dirtyInstanceIndicesGPU_.clear();

  uploadBuildOutputs(dirtyInstanceCount, true, false);
  return lastBuildSummary_;
}

SoftwareTLASManager::BuildSummary SoftwareTLASManager::refit(const std::vector<InstanceBuildInput> &instances, const std::vector<uint32_t> &dirtyInstanceIndices)
{
  if (instances.size() > settings_.maxInstances)
  {
    throw std::runtime_error("SoftwareTLASManager maxInstances exceeded");
  }

  if (instanceInputs_.empty() || instances.size() != instanceInputs_.size() || instancesGPU_.size() != lastBuildSummary_.instanceCount)
  {
    return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
  }

  std::vector<uint32_t> oldInstanceToNewInstance(instancesGPU_.size(), INVALID_INSTANCE_INDEX);
  std::vector<InstanceGPU> remappedInstancesGPU;
  std::vector<InstanceAABBGPU> remappedInstanceAabbsGPU;
  std::vector<uint32_t> remappedInputToInstance(instances.size(), INVALID_INSTANCE_INDEX);
  std::vector<uint32_t> remappedInstanceToInput;
  std::vector<uint32_t> remappedInstanceToLeaf(instancesGPU_.size(), INVALID_NODE);
  std::unordered_map<virtualgeometry::InstanceId, uint32_t> remappedInstanceIdToIndex;
  remappedInstancesGPU.reserve(instancesGPU_.size());
  remappedInstanceAabbsGPU.reserve(instanceAabbsGPU_.size());
  remappedInstanceToInput.reserve(instanceIndexToInputIndex_.size());

  for (uint32_t inputIndex = 0u; inputIndex < static_cast<uint32_t>(instances.size()); ++inputIndex)
  {
    const InstanceBuildInput &input = instances[inputIndex];
    const auto instanceIt = instanceIdToInstanceIndex_.find(input.instanceId);
    if (instanceIt == instanceIdToInstanceIndex_.end() || !isValidAabb(input.worldBounds))
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const uint32_t oldInstanceIndex = instanceIt->second;
    if (oldInstanceIndex == INVALID_INSTANCE_INDEX || oldInstanceIndex >= instancesGPU_.size() || oldInstanceIndex >= instanceAabbsGPU_.size() || oldInstanceIndex >= instanceIndexToInputIndex_.size() ||
        oldInstanceIndex >= instanceIndexToLeafNodeIndex_.size())
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const uint32_t oldInputIndex = instanceIndexToInputIndex_[oldInstanceIndex];
    if (oldInputIndex >= instanceInputs_.size())
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const InstanceBuildInput &oldInput = instanceInputs_[oldInputIndex];
    if (input.instanceId != oldInput.instanceId || input.blasHandle != oldInput.blasHandle)
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const uint32_t newInstanceIndex = static_cast<uint32_t>(remappedInstancesGPU.size());
    oldInstanceToNewInstance[oldInstanceIndex] = newInstanceIndex;
    remappedInputToInstance[inputIndex] = newInstanceIndex;
    remappedInstanceToInput.push_back(inputIndex);
    remappedInstanceIdToIndex[input.instanceId] = newInstanceIndex;

    remappedInstancesGPU.push_back(makeInstanceGPU(input));

    InstanceAABBGPU aabbGpu{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      aabbGpu.minPoint[axis] = input.worldBounds.minPoint[axis];
      aabbGpu.maxPoint[axis] = input.worldBounds.maxPoint[axis];
    }
    aabbGpu.instanceIndex = newInstanceIndex;
    aabbGpu.blasHandle = input.blasHandle;
    remappedInstanceAabbsGPU.push_back(aabbGpu);
  }

  if (remappedInstancesGPU.size() != instancesGPU_.size())
  {
    return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
  }

  remapBvh4Instances(oldInstanceToNewInstance);

  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(canonicalNodesCPU_.size()); ++nodeIndex)
  {
    CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
    if (!node.isLeaf)
    {
      continue;
    }

    if (node.instanceIndex >= oldInstanceToNewInstance.size())
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const uint32_t remappedInstanceIndex = oldInstanceToNewInstance[node.instanceIndex];
    if (remappedInstanceIndex == INVALID_INSTANCE_INDEX || remappedInstanceIndex >= remappedInstanceToLeaf.size())
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }
    node.instanceIndex = remappedInstanceIndex;
    remappedInstanceToLeaf[remappedInstanceIndex] = nodeIndex;
  }

  instanceInputs_ = instances;
  instancesGPU_ = std::move(remappedInstancesGPU);
  instanceAabbsGPU_ = std::move(remappedInstanceAabbsGPU);
  compactedInputToInstanceIndex_ = std::move(remappedInputToInstance);
  instanceIndexToInputIndex_ = std::move(remappedInstanceToInput);
  instanceIndexToLeafNodeIndex_ = std::move(remappedInstanceToLeaf);
  instanceIdToInstanceIndex_ = std::move(remappedInstanceIdToIndex);
  syncCanonicalGpuState();

  dirtyInstanceIndicesGPU_.clear();
  dirtyInstanceIndicesGPU_.reserve(dirtyInstanceIndices.size());
  std::vector<uint32_t> dirtyLeafNodeIndices;
  dirtyLeafNodeIndices.reserve(dirtyInstanceIndices.size());
  std::vector<float> preRefitSurfaceAreas;
  if (settings_.partialRebuildEnabled)
  {
    preRefitSurfaceAreas.resize(canonicalNodesCPU_.size(), 0.0f);
    for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(canonicalNodesCPU_.size()); ++nodeIndex)
    {
      preRefitSurfaceAreas[nodeIndex] = aabbSurfaceArea(canonicalNodesCPU_[nodeIndex].bounds);
    }
  }

  for (const uint32_t inputIndex : dirtyInstanceIndices)
  {
    if (inputIndex >= instances.size())
    {
      continue;
    }

    const InstanceBuildInput &newInput = instances[inputIndex];
    const auto instanceIt = instanceIdToInstanceIndex_.find(newInput.instanceId);
    if (instanceIt == instanceIdToInstanceIndex_.end() || !isValidAabb(newInput.worldBounds))
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const uint32_t instanceIndex = instanceIt->second;
    if (instanceIndex == INVALID_INSTANCE_INDEX || instanceIndex >= instancesGPU_.size() || instanceIndex >= instanceAabbsGPU_.size() || instanceIndex >= instanceIndexToInputIndex_.size() ||
        instanceIndex >= instanceIndexToLeafNodeIndex_.size() || instanceIndexToLeafNodeIndex_[instanceIndex] == INVALID_NODE || instanceIndexToLeafNodeIndex_[instanceIndex] >= canonicalNodesCPU_.size() ||
        instanceIndexToLeafNodeIndex_[instanceIndex] >= canonicalNodesGPU_.size())
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const uint32_t storedInputIndex = instanceIndexToInputIndex_[instanceIndex];
    if (storedInputIndex >= instanceInputs_.size())
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    const InstanceBuildInput &oldInput = instanceInputs_[storedInputIndex];
    if (newInput.instanceId != oldInput.instanceId || newInput.blasHandle != oldInput.blasHandle)
    {
      return rebuild(instances, static_cast<uint32_t>(dirtyInstanceIndices.size()));
    }

    instanceInputs_[storedInputIndex] = newInput;

    instancesGPU_[instanceIndex] = makeInstanceGPU(newInput);

    InstanceAABBGPU &aabbGpu = instanceAabbsGPU_[instanceIndex];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      aabbGpu.minPoint[axis] = newInput.worldBounds.minPoint[axis];
      aabbGpu.maxPoint[axis] = newInput.worldBounds.maxPoint[axis];
    }
    aabbGpu.instanceIndex = instanceIndex;
    aabbGpu.blasHandle = newInput.blasHandle;

    CanonicalNodeCPU &leafNode = canonicalNodesCPU_[instanceIndexToLeafNodeIndex_[instanceIndex]];
    leafNode.bounds = newInput.worldBounds;
    leafNode.subtreeLeafCount = 1u;
    refitCanonicalPath(instanceIndexToLeafNodeIndex_[instanceIndex]);

    dirtyInstanceIndicesGPU_.push_back(instanceIndex);
    dirtyLeafNodeIndices.push_back(instanceIndexToLeafNodeIndex_[instanceIndex]);
  }

  const uint32_t partialRebuildRootCount = rebuildDirtyCanonicalSubtrees(dirtyLeafNodeIndices, preRefitSurfaceAreas);
  if (partialRebuildRootCount > 0u)
  {
    syncCanonicalGpuState();
  }
  bool bvh4Refitted = false;
  bool bvh4RebuiltForSah = false;
  if (!refitBvh4State())
  {
    rebuildBvh4State(true);
  }
  else
  {
    bvh4Refitted = true;
    const float baseline = std::max(lastBuildSummary_.bvh4BuildSahScore, 1.0e-6f);
    const float growth = lastBuildSummary_.bvh4CurrentSahScore / baseline;
    if (growth > std::max(settings_.bvh4RefitSahGrowthThreshold, 1.0f))
    {
      rebuildBvh4State(true);
      bvh4RebuiltForSah = true;
      bvh4Refitted = false;
    }
  }
  rebuildCwbvhState();

  lastBuildSummary_.instanceCount = static_cast<uint32_t>(instancesGPU_.size());
  lastBuildSummary_.nodeCount = static_cast<uint32_t>(canonicalNodesGPU_.size());
  lastBuildSummary_.rootNodeIndex = canonicalNodesCPU_.empty() ? INVALID_NODE : 0u;
  lastBuildSummary_.dirtyInstanceCount = static_cast<uint32_t>(dirtyInstanceIndicesGPU_.size());
  lastBuildSummary_.buildRequested = false;
  lastBuildSummary_.refitRequested = !dirtyInstanceIndicesGPU_.empty();
  lastBuildSummary_.rebuilt = false;
  lastBuildSummary_.refitted = !dirtyInstanceIndicesGPU_.empty();
  lastBuildSummary_.partialRebuilt = partialRebuildRootCount > 0u;
  lastBuildSummary_.partialRebuildRootCount = partialRebuildRootCount;
  lastBuildSummary_.bvh4Rebuilt = !bvh4Refitted;
  lastBuildSummary_.bvh4Refitted = bvh4Refitted;
  lastBuildSummary_.bvh4RebuiltForSah = bvh4RebuiltForSah;
  if (lastBuildSummary_.refitted)
  {
    ++lastBuildSummary_.refitRevision;
  }

  uploadBuildOutputs(lastBuildSummary_.dirtyInstanceCount, false, lastBuildSummary_.refitted);
  return lastBuildSummary_;
}

SoftwareTLASManager::BuildSummary SoftwareTLASManager::markClean()
{
  lastBuildSummary_.dirtyInstanceCount = 0u;
  lastBuildSummary_.buildRequested = false;
  lastBuildSummary_.refitRequested = false;
  lastBuildSummary_.rebuilt = false;
  lastBuildSummary_.refitted = false;
  lastBuildSummary_.partialRebuilt = false;
  lastBuildSummary_.partialRebuildRootCount = 0u;
  lastBuildSummary_.bvh4Rebuilt = false;
  lastBuildSummary_.bvh4Refitted = false;
  lastBuildSummary_.bvh4RebuiltForSah = false;
  dirtyInstanceIndicesGPU_.clear();
  uploadIdleState(0u);
  return lastBuildSummary_;
}

SoftwareTLASManager::BuildSummary SoftwareTLASManager::clear()
{
  instanceInputs_.clear();
  canonicalNodesCPU_.clear();
  instancesGPU_.clear();
  instanceAabbsGPU_.clear();
  canonicalNodesGPU_.clear();
  cwbvhNodesGPU_.clear();
  cwbvhNodeRefsGPU_.clear();
  dobbCwbvhNodesGPU_.clear();
  cwbvhNodeMetadataGPU_.clear();
  bvh4NodesCPU_.clear();
  bvh4NodesGPU_.clear();
  leafInstanceIndicesGPU_.clear();
  dirtyInstanceIndicesGPU_.clear();
  compactedInputToInstanceIndex_.clear();
  instanceIndexToInputIndex_.clear();
  instanceIndexToLeafNodeIndex_.clear();
  instanceIdToInstanceIndex_.clear();

  lastBuildSummary_.instanceCount = 0u;
  lastBuildSummary_.nodeCount = 0u;
  lastBuildSummary_.rootNodeIndex = INVALID_NODE;
  lastBuildSummary_.dirtyInstanceCount = 0u;
  lastBuildSummary_.buildRequested = false;
  lastBuildSummary_.refitRequested = false;
  lastBuildSummary_.rebuilt = false;
  lastBuildSummary_.refitted = false;
  lastBuildSummary_.partialRebuilt = false;
  lastBuildSummary_.partialRebuildRootCount = 0u;
  lastBuildSummary_.bvh4Rebuilt = false;
  lastBuildSummary_.bvh4Refitted = false;
  lastBuildSummary_.bvh4RebuiltForSah = false;
  lastBuildSummary_.bvh4NodeCount = 0u;
  lastBuildSummary_.bvh4LeafCount = 0u;
  lastBuildSummary_.bvh4MaxDepth = 0u;
  lastBuildSummary_.bvh4BuildSahScore = 0.0f;
  lastBuildSummary_.bvh4CurrentSahScore = 0.0f;
  lastBuildSummary_.bvh4SahGrowth = 1.0f;

  uploadIdleState(0u);
  return lastBuildSummary_;
}

void SoftwareTLASManager::appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, const uint32_t frameSlot) const
{
  for (const auto &[bufferName, bufferIds] : frameOverrideBufferIds_)
  {
    if (bufferIds.empty())
    {
      continue;
    }

    overrides.bufferOverrides.emplace(
        bufferName,
        rendering::RenderGraphBufferOverride{
          .bufferId = bufferIds[frameSlot % bufferIds.size()],
        });
  }
}

void SoftwareTLASManager::initializeResources()
{
  instancesBufferSize_ = static_cast<uint64_t>(settings_.maxInstances) * sizeof(InstanceGPU);
  instanceAabbsBufferSize_ = static_cast<uint64_t>(settings_.maxInstances) * sizeof(InstanceAABBGPU);
  canonicalNodesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances * 2u - 1u)) * sizeof(CanonicalNodeGPU);
  buildHLBVHNodesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances * 2u - 1u)) * sizeof(GPUBuildHLBVHNodeGPU);
  cwbvhNodesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(CWBVHNodeGPU);
  cwbvhNodeRefsBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(CWBVHNodeRefGPU);
  dobbCwbvhNodesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(CWBVHNodeGPU);
  cwbvhNodeMetadataBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(CWBVHNodeMetadataGPU);
  bvh4NodesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(BVH4NodeGPU);
  buildMortonCodesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(uint32_t);
  buildInstanceIndicesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(uint32_t);
  buildFlagsBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances * 2u)) * sizeof(uint32_t);
  buildSignalsBufferSize_ = 64u * sizeof(uint32_t);
  buildDebugBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances * 8u)) * sizeof(uint32_t);
  leafInstanceIndicesBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxInstances)) * sizeof(uint32_t);
  buildIndirectArgsBufferSize_ = sizeof(DispatchIndirectArgsGPU);
  buildSortCountBufferSize_ = sizeof(uint32_t);
  buildSortIndirectArgsBufferSize_ = static_cast<uint64_t>(kRadixSortDispatchSlotCount) * sizeof(DispatchIndirectArgsGPU);
  refitIndirectArgsBufferSize_ = sizeof(DispatchIndirectArgsGPU);
  refitCwbvhIndirectArgsBufferSize_ = sizeof(DispatchIndirectArgsGPU);
  dirtyInstanceIndicesBufferSize_ = static_cast<uint64_t>(settings_.maxInstances) * sizeof(uint32_t);
  countersBufferSize_ = sizeof(CountersGPU);

  const rendering::BufferUsage bufferUsage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc;

  instancesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_Instances.buffer",
        .size = instancesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  instanceAabbsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_InstanceAABBs.buffer",
        .size = instanceAabbsBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  canonicalNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_CanonicalNodes.buffer",
        .size = canonicalNodesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildHLBVHNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildHLBVHNodes.buffer",
        .size = buildHLBVHNodesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  cwbvhNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_CWBVHNodes.buffer",
        .size = cwbvhNodesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  cwbvhNodeRefsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_CWBVHNodeRefs.buffer",
        .size = cwbvhNodeRefsBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  dobbCwbvhNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_DOBBCWBVHNodes.buffer",
        .size = dobbCwbvhNodesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  cwbvhNodeMetadataBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_CWBVHNodeMetadata.buffer",
        .size = cwbvhNodeMetadataBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  bvh4NodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BVH4Nodes.buffer",
        .size = bvh4NodesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildMortonCodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildMortonCodes.buffer",
        .size = buildMortonCodesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildInstanceIndicesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildInstanceIndices.buffer",
        .size = buildInstanceIndicesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildFlagsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildFlags.buffer",
        .size = buildFlagsBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildSignalsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildSignals.buffer",
        .size = buildSignalsBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildDebugBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildDebug.buffer",
        .size = buildDebugBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  leafInstanceIndicesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_LeafInstanceIndices.buffer",
        .size = leafInstanceIndicesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildIndirectArgsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildIndirectArgs.buffer",
        .size = buildIndirectArgsBufferSize_,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      settings_.frameLocalBuffers);

  buildSortCountBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildSortCount.buffer",
        .size = buildSortCountBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  buildSortIndirectArgsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_BuildSortIndirectArgs.buffer",
        .size = buildSortIndirectArgsBufferSize_,
        .usage = rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      settings_.frameLocalBuffers);

  refitIndirectArgsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_RefitIndirectArgs.buffer",
        .size = refitIndirectArgsBufferSize_,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      settings_.frameLocalBuffers);

  refitCwbvhIndirectArgsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_RefitCWBVHIndirectArgs.buffer",
        .size = refitCwbvhIndirectArgsBufferSize_,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      settings_.frameLocalBuffers);

  dirtyInstanceIndicesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_DirtyInstanceIndices.buffer",
        .size = dirtyInstanceIndicesBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);

  countersBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
        .name = "SoftwareTLAS_Counters.buffer",
        .size = countersBufferSize_,
        .usage = bufferUsage,
      },
      settings_.frameLocalBuffers);
}

rendering::Buffer SoftwareTLASManager::createManagedBuffer(const rendering::BufferInfo info, const bool frameLocal)
{
  const rendering::Buffer buffer = renderGraph_->createBuffer(info);
  if (frameLocal)
  {
    initializeFrameOverrideBuffer(buffer, info.size, info.usage);
  }
  return buffer;
}

void SoftwareTLASManager::initializeFrameOverrideBuffer(const rendering::Buffer &buffer, const uint64_t size, const rendering::BufferUsage usage)
{
  const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
  if (frameCount <= 1u)
  {
    return;
  }

  auto &bufferIds = frameOverrideBufferIds_[buffer.name];
  if (!bufferIds.empty())
  {
    return;
  }

  bufferIds.assign(frameCount, rendering::BufferId::Invalid);
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    bufferIds[frameSlot] = renderGraph_->getRHI()->createBuffer(
        rendering::BufferInfo{
          .name = buffer.name + ".override_frame" + std::to_string(frameSlot),
          .size = size,
          .usage = usage,
        });
  }
}

rendering::BufferId SoftwareTLASManager::getFrameOverrideBufferId(const rendering::Buffer &buffer, const uint32_t frameSlot) const
{
  const auto it = frameOverrideBufferIds_.find(buffer.name);
  if (it == frameOverrideBufferIds_.end() || it->second.empty())
  {
    return rendering::BufferId::Invalid;
  }

  return it->second[frameSlot % it->second.size()];
}

void SoftwareTLASManager::writeManagedBuffer(const rendering::Buffer &buffer, const uint64_t offset, const uint64_t size, const void *data) const
{
  if (buffer.isValid() == false || data == nullptr || size == 0u)
  {
    return;
  }

  const rendering::BufferId overrideBufferId = getFrameOverrideBufferId(buffer, renderGraph_->getCurrentFrameIndex());
  if (overrideBufferId != rendering::BufferId::Invalid)
  {
    renderGraph_->getRHI()->bufferWrite(overrideBufferId, offset, size, const_cast<void *>(data));
    return;
  }

  renderGraph_->bufferWrite(buffer, offset, size, const_cast<void *>(data));
}

void SoftwareTLASManager::uploadIdleState(const uint32_t dirtyInstanceCount)
{
  const DispatchIndirectArgsGPU buildDispatchArgs{};
  const std::array<DispatchIndirectArgsGPU, kRadixSortDispatchSlotCount> buildSortDispatchArgs{};
  const DispatchIndirectArgsGPU refitDispatchArgs{};
  const DispatchIndirectArgsGPU refitCwbvhDispatchArgs{};
  const uint32_t buildSortCount = 0u;
  const uint32_t invalidDirtyInstanceIndex = INVALID_INSTANCE_INDEX;
  CountersGPU counters{};
  counters.instanceCount = lastBuildSummary_.instanceCount;
  counters.nodeCount = lastBuildSummary_.nodeCount;
  counters.rootNodeIndex = lastBuildSummary_.rootNodeIndex;
  counters.buildRevision = lastBuildSummary_.buildRevision;
  counters.refitRevision = lastBuildSummary_.refitRevision;
  counters.dirtyInstanceCount = dirtyInstanceCount;
  counters.buildRequested = 0u;
  counters.refitRequested = 0u;

  writeManagedBuffer(buildIndirectArgsBuffer_, 0u, sizeof(buildDispatchArgs), &buildDispatchArgs);
  writeManagedBuffer(buildSortCountBuffer_, 0u, sizeof(buildSortCount), &buildSortCount);
  writeManagedBuffer(buildSortIndirectArgsBuffer_, 0u, sizeof(buildSortDispatchArgs), buildSortDispatchArgs.data());
  writeManagedBuffer(refitIndirectArgsBuffer_, 0u, sizeof(refitDispatchArgs), &refitDispatchArgs);
  writeManagedBuffer(refitCwbvhIndirectArgsBuffer_, 0u, sizeof(refitCwbvhDispatchArgs), &refitCwbvhDispatchArgs);
  writeManagedBuffer(dirtyInstanceIndicesBuffer_, 0u, sizeof(invalidDirtyInstanceIndex), &invalidDirtyInstanceIndex);
  writeManagedBuffer(countersBuffer_, 0u, sizeof(counters), &counters);
}

void SoftwareTLASManager::uploadBuildOutputs(const uint32_t dirtyInstanceCount, const bool rebuilt, const bool refitted)
{
  if (!instancesGPU_.empty())
  {
    writeManagedBuffer(instancesBuffer_, 0u, instancesGPU_.size() * sizeof(InstanceGPU), instancesGPU_.data());
  }

  if (!instanceAabbsGPU_.empty())
  {
    writeManagedBuffer(instanceAabbsBuffer_, 0u, instanceAabbsGPU_.size() * sizeof(InstanceAABBGPU), instanceAabbsGPU_.data());
  }

  if (!canonicalNodesGPU_.empty())
  {
    writeManagedBuffer(canonicalNodesBuffer_, 0u, canonicalNodesGPU_.size() * sizeof(CanonicalNodeGPU), canonicalNodesGPU_.data());
  }

  if (!cwbvhNodesGPU_.empty())
  {
    writeManagedBuffer(cwbvhNodesBuffer_, 0u, cwbvhNodesGPU_.size() * sizeof(CWBVHNodeGPU), cwbvhNodesGPU_.data());
  }

  if (!cwbvhNodeRefsGPU_.empty())
  {
    writeManagedBuffer(cwbvhNodeRefsBuffer_, 0u, cwbvhNodeRefsGPU_.size() * sizeof(CWBVHNodeRefGPU), cwbvhNodeRefsGPU_.data());
  }

  if (!dobbCwbvhNodesGPU_.empty())
  {
    writeManagedBuffer(dobbCwbvhNodesBuffer_, 0u, dobbCwbvhNodesGPU_.size() * sizeof(CWBVHNodeGPU), dobbCwbvhNodesGPU_.data());
  }

  if (!cwbvhNodeMetadataGPU_.empty())
  {
    writeManagedBuffer(cwbvhNodeMetadataBuffer_, 0u, cwbvhNodeMetadataGPU_.size() * sizeof(CWBVHNodeMetadataGPU), cwbvhNodeMetadataGPU_.data());
  }

  if (!bvh4NodesGPU_.empty())
  {
    writeManagedBuffer(bvh4NodesBuffer_, 0u, bvh4NodesGPU_.size() * sizeof(BVH4NodeGPU), bvh4NodesGPU_.data());
  }

  if (!leafInstanceIndicesGPU_.empty())
  {
    writeManagedBuffer(leafInstanceIndicesBuffer_, 0u, leafInstanceIndicesGPU_.size() * sizeof(uint32_t), leafInstanceIndicesGPU_.data());
  }

  if (!dirtyInstanceIndicesGPU_.empty())
  {
    writeManagedBuffer(dirtyInstanceIndicesBuffer_, 0u, dirtyInstanceIndicesGPU_.size() * sizeof(uint32_t), dirtyInstanceIndicesGPU_.data());
  }
  else
  {
    const uint32_t invalidDirtyInstanceIndex = INVALID_INSTANCE_INDEX;
    writeManagedBuffer(dirtyInstanceIndicesBuffer_, 0u, sizeof(invalidDirtyInstanceIndex), &invalidDirtyInstanceIndex);
  }

  DispatchIndirectArgsGPU buildDispatchArgs{};
  uint32_t buildSortCount = 0u;
  std::array<DispatchIndirectArgsGPU, kRadixSortDispatchSlotCount> buildSortDispatchArgs{};
  if (rebuilt && lastBuildSummary_.instanceCount > 0u)
  {
    buildDispatchArgs.groupCountX = divCeilU32(lastBuildSummary_.instanceCount, settings_.workgroupSize);
    buildSortCount = lastBuildSummary_.instanceCount;

    const uint32_t sortNumWgs = divCeilU32(buildSortCount, kRadixSortBlockSize);
    const uint32_t sortCountEntries = sortNumWgs * kRadixSortBinCount;
    const uint32_t sortReduceWgs = kRadixSortBinCount * divCeilU32(sortNumWgs, kRadixSortBlockSize);
    buildSortDispatchArgs[0u].groupCountX = divCeilU32(sortCountEntries, kRadixSortWorkgroupSize);
    buildSortDispatchArgs[1u].groupCountX = sortNumWgs;
    buildSortDispatchArgs[2u].groupCountX = sortReduceWgs;
    buildSortDispatchArgs[3u].groupCountX = sortReduceWgs > 0u ? 1u : 0u;
    buildSortDispatchArgs[4u].groupCountX = sortReduceWgs;
    buildSortDispatchArgs[5u].groupCountX = sortNumWgs;
  }

  DispatchIndirectArgsGPU refitDispatchArgs{};
  DispatchIndirectArgsGPU refitCwbvhDispatchArgs{};
  if (refitted && dirtyInstanceCount > 0u)
  {
    refitDispatchArgs.groupCountX = divCeilU32(dirtyInstanceCount, settings_.workgroupSize);
    refitCwbvhDispatchArgs.groupCountX = 0u;
  }

  CountersGPU counters{};
  counters.instanceCount = lastBuildSummary_.instanceCount;
  counters.nodeCount = lastBuildSummary_.nodeCount;
  counters.rootNodeIndex = lastBuildSummary_.rootNodeIndex;
  counters.buildRevision = lastBuildSummary_.buildRevision;
  counters.refitRevision = lastBuildSummary_.refitRevision;
  counters.dirtyInstanceCount = dirtyInstanceCount;
  counters.buildRequested = rebuilt ? 1u : 0u;
  counters.refitRequested = refitted ? 1u : 0u;

  writeManagedBuffer(buildIndirectArgsBuffer_, 0u, sizeof(buildDispatchArgs), &buildDispatchArgs);
  writeManagedBuffer(buildSortCountBuffer_, 0u, sizeof(buildSortCount), &buildSortCount);
  writeManagedBuffer(buildSortIndirectArgsBuffer_, 0u, sizeof(buildSortDispatchArgs), buildSortDispatchArgs.data());
  writeManagedBuffer(refitIndirectArgsBuffer_, 0u, sizeof(refitDispatchArgs), &refitDispatchArgs);
  writeManagedBuffer(refitCwbvhIndirectArgsBuffer_, 0u, sizeof(refitCwbvhDispatchArgs), &refitCwbvhDispatchArgs);
  writeManagedBuffer(countersBuffer_, 0u, sizeof(counters), &counters);
}

uint32_t SoftwareTLASManager::rebuildGpuState(const std::vector<InstanceBuildInput> &instances)
{
  instanceInputs_ = instances;
  canonicalNodesCPU_.clear();
  instancesGPU_.clear();
  instanceAabbsGPU_.clear();
  canonicalNodesGPU_.clear();
  cwbvhNodesGPU_.clear();
  cwbvhNodeRefsGPU_.clear();
  dobbCwbvhNodesGPU_.clear();
  cwbvhNodeMetadataGPU_.clear();
  bvh4NodesCPU_.clear();
  bvh4NodesGPU_.clear();
  leafInstanceIndicesGPU_.clear();
  dirtyInstanceIndicesGPU_.clear();
  compactedInputToInstanceIndex_.assign(instanceInputs_.size(), INVALID_INSTANCE_INDEX);
  instanceIndexToInputIndex_.clear();
  instanceIndexToLeafNodeIndex_.clear();
  instanceIdToInstanceIndex_.clear();

  instancesGPU_.reserve(instanceInputs_.size());
  instanceAabbsGPU_.reserve(instanceInputs_.size());
  instanceIndexToInputIndex_.reserve(instanceInputs_.size());

  std::vector<BuildPrimitive> primitives;
  primitives.reserve(instanceInputs_.size());

  for (uint32_t inputIndex = 0u; inputIndex < static_cast<uint32_t>(instanceInputs_.size()); ++inputIndex)
  {
    const InstanceBuildInput &input = instanceInputs_[inputIndex];
    if (!isValidAabb(input.worldBounds))
    {
      continue;
    }

    const uint32_t gpuInstanceIndex = static_cast<uint32_t>(instancesGPU_.size());
    compactedInputToInstanceIndex_[inputIndex] = gpuInstanceIndex;
    instanceIndexToInputIndex_.push_back(inputIndex);
    instanceIndexToLeafNodeIndex_.push_back(INVALID_NODE);
    instanceIdToInstanceIndex_[input.instanceId] = gpuInstanceIndex;

    instancesGPU_.push_back(makeInstanceGPU(input));

    InstanceAABBGPU aabbGpu{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      aabbGpu.minPoint[axis] = input.worldBounds.minPoint[axis];
      aabbGpu.maxPoint[axis] = input.worldBounds.maxPoint[axis];
    }
    aabbGpu.instanceIndex = gpuInstanceIndex;
    aabbGpu.blasHandle = input.blasHandle;
    instanceAabbsGPU_.push_back(aabbGpu);

    BuildPrimitive primitive{};
    primitive.instanceIndex = gpuInstanceIndex;
    primitive.rotationCandidateIndex = closestDobbRotationForTransform(input.transform);
    primitive.dobbProxy = computeAabbDobbProjectionProxy(input.worldBounds);
    primitive.bounds = input.worldBounds;
    computeCentroid(input.worldBounds, primitive.centroid);
    primitives.push_back(primitive);
  }

  uint32_t rootNodeIndex = INVALID_NODE;
  if (!primitives.empty())
  {
    canonicalNodesCPU_.reserve(primitives.size() * 2u - 1u);
    rootNodeIndex = buildCanonicalNodeRecursive(primitives, 0u, static_cast<uint32_t>(primitives.size()), INVALID_NODE, 0u);
  }

  canonicalNodesGPU_.reserve(canonicalNodesCPU_.size());
  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(canonicalNodesCPU_.size()); ++nodeIndex)
  {
    const CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
    CanonicalNodeGPU gpuNode{};
    gpuNode.parent = node.parent;
    gpuNode.left = node.left;
    gpuNode.right = node.right;
    gpuNode.instanceIndex = node.instanceIndex;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      gpuNode.minPoint[axis] = node.bounds.minPoint[axis];
      gpuNode.maxPoint[axis] = node.bounds.maxPoint[axis];
    }
    gpuNode.flags = (node.isLeaf ? NODE_FLAG_LEAF : 0u) | (node.depth << NODE_DEPTH_SHIFT);
    gpuNode.subtreeLeafCount = node.subtreeLeafCount;
    canonicalNodesGPU_.push_back(gpuNode);

    if (node.isLeaf && node.instanceIndex < instanceIndexToLeafNodeIndex_.size())
    {
      instanceIndexToLeafNodeIndex_[node.instanceIndex] = nodeIndex;
    }
  }

  rebuildBvh4State(true);
  rebuildCwbvhState();

  return rootNodeIndex;
}

void SoftwareTLASManager::refitCanonicalPath(const uint32_t nodeIndex)
{
  if (nodeIndex >= canonicalNodesCPU_.size())
  {
    return;
  }

  uint32_t currentNodeIndex = nodeIndex;
  while (currentNodeIndex != INVALID_NODE)
  {
    CanonicalNodeCPU &cpuNode = canonicalNodesCPU_[currentNodeIndex];
    CanonicalNodeGPU &gpuNode = canonicalNodesGPU_[currentNodeIndex];

    if (!cpuNode.isLeaf)
    {
      bool hasBounds = false;
      virtualgeometry::AABB bounds{};
      uint32_t subtreeLeafCount = 0u;

      if (cpuNode.left != INVALID_NODE && cpuNode.left < canonicalNodesCPU_.size())
      {
        bounds = canonicalNodesCPU_[cpuNode.left].bounds;
        subtreeLeafCount += canonicalNodesCPU_[cpuNode.left].subtreeLeafCount;
        hasBounds = true;
      }

      if (cpuNode.right != INVALID_NODE && cpuNode.right < canonicalNodesCPU_.size())
      {
        bounds = hasBounds ? unionBounds(bounds, canonicalNodesCPU_[cpuNode.right].bounds) : canonicalNodesCPU_[cpuNode.right].bounds;
        subtreeLeafCount += canonicalNodesCPU_[cpuNode.right].subtreeLeafCount;
        hasBounds = true;
      }

      if (hasBounds)
      {
        cpuNode.bounds = bounds;
        cpuNode.subtreeLeafCount = subtreeLeafCount;
      }
    }

    gpuNode.parent = cpuNode.parent;
    gpuNode.left = cpuNode.left;
    gpuNode.right = cpuNode.right;
    gpuNode.instanceIndex = cpuNode.instanceIndex;
    gpuNode.flags = (cpuNode.isLeaf ? NODE_FLAG_LEAF : 0u) | (cpuNode.depth << NODE_DEPTH_SHIFT);
    gpuNode.subtreeLeafCount = cpuNode.subtreeLeafCount;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      gpuNode.minPoint[axis] = cpuNode.bounds.minPoint[axis];
      gpuNode.maxPoint[axis] = cpuNode.bounds.maxPoint[axis];
    }

    currentNodeIndex = cpuNode.parent;
  }
}

void SoftwareTLASManager::syncCanonicalGpuNode(const uint32_t nodeIndex)
{
  if (nodeIndex >= canonicalNodesCPU_.size() || nodeIndex >= canonicalNodesGPU_.size())
  {
    return;
  }

  const CanonicalNodeCPU &cpuNode = canonicalNodesCPU_[nodeIndex];
  CanonicalNodeGPU &gpuNode = canonicalNodesGPU_[nodeIndex];
  gpuNode.parent = cpuNode.parent;
  gpuNode.left = cpuNode.left;
  gpuNode.right = cpuNode.right;
  gpuNode.instanceIndex = cpuNode.instanceIndex;
  gpuNode.flags = (cpuNode.isLeaf ? NODE_FLAG_LEAF : 0u) | (cpuNode.depth << NODE_DEPTH_SHIFT);
  gpuNode.subtreeLeafCount = cpuNode.subtreeLeafCount;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    gpuNode.minPoint[axis] = cpuNode.bounds.minPoint[axis];
    gpuNode.maxPoint[axis] = cpuNode.bounds.maxPoint[axis];
  }
}

void SoftwareTLASManager::syncCanonicalGpuState()
{
  if (canonicalNodesGPU_.size() != canonicalNodesCPU_.size())
  {
    canonicalNodesGPU_.resize(canonicalNodesCPU_.size());
  }

  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(canonicalNodesCPU_.size()); ++nodeIndex)
  {
    syncCanonicalGpuNode(nodeIndex);
  }
}

uint32_t SoftwareTLASManager::rebuildDirtyCanonicalSubtrees(const std::vector<uint32_t> &dirtyLeafNodeIndices, const std::vector<float> &preRefitSurfaceAreas)
{
  if (!settings_.partialRebuildEnabled || dirtyLeafNodeIndices.empty() || canonicalNodesCPU_.empty() || preRefitSurfaceAreas.size() != canonicalNodesCPU_.size())
  {
    return 0u;
  }

  std::vector<uint32_t> dirtyLeafCounts(canonicalNodesCPU_.size(), 0u);
  for (const uint32_t dirtyLeafNodeIndex : dirtyLeafNodeIndices)
  {
    if (dirtyLeafNodeIndex >= canonicalNodesCPU_.size())
    {
      continue;
    }

    uint32_t nodeIndex = dirtyLeafNodeIndex;
    while (nodeIndex != INVALID_NODE && nodeIndex < canonicalNodesCPU_.size())
    {
      ++dirtyLeafCounts[nodeIndex];
      nodeIndex = canonicalNodesCPU_[nodeIndex].parent;
    }
  }

  struct Candidate
  {
    uint32_t nodeIndex = INVALID_NODE;
    uint32_t depth = 0u;
  };

  std::vector<Candidate> candidates;
  candidates.reserve(dirtyLeafNodeIndices.size());
  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(canonicalNodesCPU_.size()); ++nodeIndex)
  {
    const CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
    if (node.isLeaf || dirtyLeafCounts[nodeIndex] == 0u || node.subtreeLeafCount < settings_.partialRebuildMinLeafCount)
    {
      continue;
    }

    const float dirtyRatio = node.subtreeLeafCount > 0u ? static_cast<float>(dirtyLeafCounts[nodeIndex]) / static_cast<float>(node.subtreeLeafCount) : 0.0f;
    const float oldArea = preRefitSurfaceAreas[nodeIndex];
    const float newArea = aabbSurfaceArea(node.bounds);
    const bool dirtyEnough = dirtyRatio >= settings_.partialRebuildDirtyRatioThreshold;
    const bool sahDegraded = oldArea > 0.0f && newArea > oldArea * settings_.partialRebuildSahGrowthThreshold;

    if (dirtyEnough || sahDegraded)
    {
      candidates.push_back(Candidate{.nodeIndex = nodeIndex, .depth = node.depth});
    }
  }

  std::sort(
      candidates.begin(),
      candidates.end(),
      [](const Candidate &a, const Candidate &b)
      {
        if (a.depth != b.depth)
        {
          return a.depth > b.depth;
        }
        return a.nodeIndex < b.nodeIndex;
      });

  std::vector<uint8_t> covered(canonicalNodesCPU_.size(), 0u);
  std::vector<uint32_t> selectedRoots;
  selectedRoots.reserve(candidates.size());
  for (const Candidate &candidate : candidates)
  {
    if (candidate.nodeIndex >= covered.size() || covered[candidate.nodeIndex] != 0u)
    {
      continue;
    }

    selectedRoots.push_back(candidate.nodeIndex);
    std::vector<uint32_t> subtreeNodes;
    collectSubtreeNodeIndices(candidate.nodeIndex, subtreeNodes);
    for (const uint32_t subtreeNodeIndex : subtreeNodes)
    {
      if (subtreeNodeIndex < covered.size())
      {
        covered[subtreeNodeIndex] = 1u;
      }
    }

    uint32_t ancestorIndex = canonicalNodesCPU_[candidate.nodeIndex].parent;
    while (ancestorIndex != INVALID_NODE && ancestorIndex < covered.size())
    {
      covered[ancestorIndex] = 1u;
      ancestorIndex = canonicalNodesCPU_[ancestorIndex].parent;
    }
  }

  for (const uint32_t rootNodeIndex : selectedRoots)
  {
    rebuildCanonicalSubtree(rootNodeIndex);
  }

  return static_cast<uint32_t>(selectedRoots.size());
}

void SoftwareTLASManager::collectSubtreeNodeIndices(const uint32_t nodeIndex, std::vector<uint32_t> &nodeIndices) const
{
  if (nodeIndex >= canonicalNodesCPU_.size())
  {
    return;
  }

  nodeIndices.push_back(nodeIndex);
  const CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
  if (node.left != INVALID_NODE)
  {
    collectSubtreeNodeIndices(node.left, nodeIndices);
  }
  if (node.right != INVALID_NODE && node.right != node.left)
  {
    collectSubtreeNodeIndices(node.right, nodeIndices);
  }
}

void SoftwareTLASManager::collectSubtreePrimitives(const uint32_t nodeIndex, std::vector<BuildPrimitive> &primitives) const
{
  if (nodeIndex >= canonicalNodesCPU_.size())
  {
    return;
  }

  const CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
  if (node.isLeaf)
  {
    if (node.instanceIndex < instanceAabbsGPU_.size())
    {
      BuildPrimitive primitive{};
      primitive.instanceIndex = node.instanceIndex;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        primitive.bounds.minPoint[axis] = instanceAabbsGPU_[node.instanceIndex].minPoint[axis];
        primitive.bounds.maxPoint[axis] = instanceAabbsGPU_[node.instanceIndex].maxPoint[axis];
      }
      computeCentroid(primitive.bounds, primitive.centroid);
      primitives.push_back(primitive);
    }
    return;
  }

  if (node.left != INVALID_NODE)
  {
    collectSubtreePrimitives(node.left, primitives);
  }
  if (node.right != INVALID_NODE && node.right != node.left)
  {
    collectSubtreePrimitives(node.right, primitives);
  }
}

uint32_t SoftwareTLASManager::rebuildCanonicalNodeRecursiveInPlace(
    std::vector<BuildPrimitive> &primitives,
    const uint32_t begin,
    const uint32_t end,
    const uint32_t parent,
    const uint32_t depth,
    const std::vector<uint32_t> &nodePool,
    uint32_t &nodePoolCursor)
{
  if (begin >= end || nodePoolCursor >= nodePool.size())
  {
    return INVALID_NODE;
  }

  const uint32_t nodeIndex = nodePool[nodePoolCursor++];
  CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
  node = CanonicalNodeCPU{};
  node.parent = parent;
  node.depth = depth;

  if (end - begin == 1u)
  {
    node.bounds = primitives[begin].bounds;
    node.instanceIndex = primitives[begin].instanceIndex;
    node.left = INVALID_NODE;
    node.right = INVALID_NODE;
    node.subtreeLeafCount = 1u;
    node.isLeaf = true;
    if (node.instanceIndex < instanceIndexToLeafNodeIndex_.size())
    {
      instanceIndexToLeafNodeIndex_[node.instanceIndex] = nodeIndex;
    }
    return nodeIndex;
  }

  const BVH4Split split = findBvh4Split(primitives, begin, end);
  const uint32_t mid = split.valid ? split.mid : begin + ((end - begin) >> 1u);

  const uint32_t left = rebuildCanonicalNodeRecursiveInPlace(primitives, begin, mid, nodeIndex, depth + 1u, nodePool, nodePoolCursor);
  const uint32_t right = rebuildCanonicalNodeRecursiveInPlace(primitives, mid, end, nodeIndex, depth + 1u, nodePool, nodePoolCursor);

  node.left = left;
  node.right = right;
  node.instanceIndex = INVALID_INSTANCE_INDEX;
  node.isLeaf = false;
  node.subtreeLeafCount = 0u;

  if (left != INVALID_NODE)
  {
    node.bounds = canonicalNodesCPU_[left].bounds;
    node.subtreeLeafCount += canonicalNodesCPU_[left].subtreeLeafCount;
  }
  if (right != INVALID_NODE)
  {
    node.bounds = left == INVALID_NODE ? canonicalNodesCPU_[right].bounds : unionBounds(node.bounds, canonicalNodesCPU_[right].bounds);
    node.subtreeLeafCount += canonicalNodesCPU_[right].subtreeLeafCount;
  }

  return nodeIndex;
}

void SoftwareTLASManager::rebuildCanonicalSubtree(const uint32_t rootNodeIndex)
{
  if (rootNodeIndex >= canonicalNodesCPU_.size() || canonicalNodesCPU_[rootNodeIndex].isLeaf)
  {
    return;
  }

  std::vector<uint32_t> nodePool;
  collectSubtreeNodeIndices(rootNodeIndex, nodePool);

  std::vector<BuildPrimitive> primitives;
  primitives.reserve(canonicalNodesCPU_[rootNodeIndex].subtreeLeafCount);
  collectSubtreePrimitives(rootNodeIndex, primitives);
  if (primitives.empty() || nodePool.size() != primitives.size() * 2u - 1u)
  {
    return;
  }

  const uint32_t parent = canonicalNodesCPU_[rootNodeIndex].parent;
  const uint32_t depth = canonicalNodesCPU_[rootNodeIndex].depth;
  uint32_t nodePoolCursor = 0u;
  const uint32_t rebuiltRoot = rebuildCanonicalNodeRecursiveInPlace(primitives, 0u, static_cast<uint32_t>(primitives.size()), parent, depth, nodePool, nodePoolCursor);
  if (rebuiltRoot != rootNodeIndex)
  {
    return;
  }

  refitCanonicalPath(rootNodeIndex);
}

void SoftwareTLASManager::rebuildCwbvhState()
{
  cwbvhNodesGPU_.clear();
  cwbvhNodeRefsGPU_.clear();
  dobbCwbvhNodesGPU_.clear();
  cwbvhNodeMetadataGPU_.clear();
  leafInstanceIndicesGPU_.clear();

  if (bvh4NodesCPU_.empty())
  {
    return;
  }

  cwbvhNodesGPU_.push_back(CWBVHNodeGPU{});
  cwbvhNodeRefsGPU_.push_back(CWBVHNodeRefGPU{});
  if (settings_.buildDobbCwbvhNodes)
  {
    dobbCwbvhNodesGPU_.push_back(CWBVHNodeGPU{});
    cwbvhNodeMetadataGPU_.push_back(CWBVHNodeMetadataGPU{});
  }
  packCwbvhFromBvh4Node(0u, 0u);
}

void SoftwareTLASManager::encodePackedCwbvhNode(
    const std::vector<PackedCwbvhChild> &frontier,
    const virtualgeometry::AABB &sourceBounds,
    const uint32_t internalBaseIndex,
    const uint32_t leafBaseIndex,
    const uint32_t depth,
    const bool useDobb,
    CWBVHNodeGPU &outNode,
    CWBVHNodeMetadataGPU &outMetadata) const
{
  outNode = CWBVHNodeGPU{};
  outMetadata = CWBVHNodeMetadataGPU{};
  outMetadata.flags = kTlasDobbNodeFlagQuantized;
  outMetadata.quantizedFlags = 1u;

  std::array<virtualgeometry::AABB, 8u> childBounds{};
  virtualgeometry::AABB parentBounds = sourceBounds;
  uint32_t rotationIndex = 0u;
  bool oriented = false;

  for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
  {
    childBounds[childSlot] = frontier[childSlot].bounds;
  }

  if (useDobb &&
      settings_.dobbRotationSet == kTlasDobbFixed104RotationSet &&
      depth <= settings_.dobbMaxConvertedLevels)
  {
    float baselineCost = 0.0f;
    float largestCandidateChildArea = -1.0f;
    uint32_t candidateRotationIndex = 0u;
    for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
    {
      if (frontier[childSlot].index != INVALID_NODE)
      {
        const float childArea = aabbSurfaceArea(frontier[childSlot].bounds);
        baselineCost += childArea;
        if (frontier[childSlot].rotationCandidateIndex != 0u && childArea > largestCandidateChildArea)
        {
          largestCandidateChildArea = childArea;
          candidateRotationIndex = frontier[childSlot].rotationCandidateIndex;
        }
      }
    }

    float bestCost = baselineCost;
    uint32_t bestRotationIndex = 0u;
    std::array<virtualgeometry::AABB, 8u> bestChildBounds = childBounds;
    virtualgeometry::AABB bestParentBounds = parentBounds;
    if (candidateRotationIndex != 0u && (candidateRotationIndex % kTlasDobbAngleCount) != 0u)
    {
      std::array<virtualgeometry::AABB, 8u> rotatedChildBounds{};
      virtualgeometry::AABB rotatedParentBounds = emptyAabb();
      float rotatedCost = 0.0f;
      bool hasChild = false;
      for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
      {
        if (frontier[childSlot].index == INVALID_NODE)
        {
          continue;
        }
        const DobbProjectionProxy childProxy = frontier[childSlot].dobbProxy.valid != 0u ? frontier[childSlot].dobbProxy : computeAabbDobbProjectionProxy(frontier[childSlot].bounds);
        rotatedChildBounds[childSlot] = refitDobbProjectionProxyInRotatedSpace(childProxy, candidateRotationIndex);
        rotatedParentBounds = hasChild ? unionBoundsLocal(rotatedParentBounds, rotatedChildBounds[childSlot]) : rotatedChildBounds[childSlot];
        rotatedCost += aabbSurfaceArea(rotatedChildBounds[childSlot]);
        hasChild = true;
      }

      if (hasChild && rotatedCost < bestCost)
      {
        bestCost = rotatedCost;
        bestRotationIndex = candidateRotationIndex;
        bestChildBounds = rotatedChildBounds;
        bestParentBounds = rotatedParentBounds;
      }
    }

    const float acceptanceAlpha = std::clamp(settings_.dobbAcceptanceAlpha, 0.0f, 1.0f);
    if (bestRotationIndex != 0u && baselineCost > 0.0f && bestCost <= baselineCost * acceptanceAlpha)
    {
      rotationIndex = bestRotationIndex;
      childBounds = bestChildBounds;
      parentBounds = bestParentBounds;
      oriented = true;
    }
  }

  const float parentMinX = parentBounds.minPoint[0];
  const float parentMinY = parentBounds.minPoint[1];
  const float parentMinZ = parentBounds.minPoint[2];
  const uint32_t sxExp = encodeSignedExponent8(parentBounds.maxPoint[0] - parentBounds.minPoint[0]);
  const uint32_t syExp = encodeSignedExponent8(parentBounds.maxPoint[1] - parentBounds.minPoint[1]);
  const uint32_t szExp = encodeSignedExponent8(parentBounds.maxPoint[2] - parentBounds.minPoint[2]);
  const float scaleX = decodeExponentScale(sxExp);
  const float scaleY = decodeExponentScale(syExp);
  const float scaleZ = decodeExponentScale(szExp);

  uint32_t leafOffset = 0u;
  uint32_t imask = 0u;
  for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
  {
    const PackedCwbvhChild &child = frontier[childSlot];
    if (child.index == INVALID_NODE)
    {
      continue;
    }

    const uint32_t arrayIndex = childSlot >> 2u;
    const uint32_t bitOffset = (childSlot & 3u) * 8u;
    const virtualgeometry::AABB &bounds = childBounds[childSlot];
    outNode.qlox[arrayIndex] |= quantizeLowerBound(bounds.minPoint[0], parentMinX, scaleX) << bitOffset;
    outNode.qloy[arrayIndex] |= quantizeLowerBound(bounds.minPoint[1], parentMinY, scaleY) << bitOffset;
    outNode.qloz[arrayIndex] |= quantizeLowerBound(bounds.minPoint[2], parentMinZ, scaleZ) << bitOffset;
    outNode.qhix[arrayIndex] |= quantizeUpperBound(bounds.maxPoint[0], parentMinX, scaleX) << bitOffset;
    outNode.qhiy[arrayIndex] |= quantizeUpperBound(bounds.maxPoint[1], parentMinY, scaleY) << bitOffset;
    outNode.qhiz[arrayIndex] |= quantizeUpperBound(bounds.maxPoint[2], parentMinZ, scaleZ) << bitOffset;

    if (!child.isLeaf)
    {
      imask |= (1u << childSlot);
      const uint32_t metaField = (1u << 5u) | ((childSlot + 24u) & 0x1fu);
      outNode.info[arrayIndex] |= metaField << bitOffset;
    }
    else
    {
      const uint32_t metaField = (1u << 5u) | (leafOffset & 0x1fu);
      outNode.info[arrayIndex] |= metaField << bitOffset;
      ++leafOffset;
    }
  }

  outNode.orxy[0] = bitCastU32(parentMinX);
  outNode.orxy[1] = bitCastU32(parentMinY);
  outNode.oeim[0] = bitCastU32(parentMinZ);
  outNode.oeim[1] = pack4xU8(sxExp, syExp, szExp, imask);
  outNode.ntbi[0] = internalBaseIndex;
  outNode.ntbi[1] = leafBaseIndex;
  outMetadata.rotationIndex = rotationIndex;
  if (oriented)
  {
    outMetadata.flags |= kTlasDobbNodeFlagOriented;
  }
}

void SoftwareTLASManager::packCwbvhFromBvh4Node(const uint32_t bvh4NodeIndex, const uint32_t packedNodeIndex)
{
  struct PendingNode
  {
    uint32_t bvh4NodeIndex = INVALID_NODE;
    uint32_t packedNodeIndex = INVALID_NODE;
  };

  auto makeChild = [&](const BVH4ChildCPU &child)
  {
    return PackedCwbvhChild{
      .bounds = child.bounds,
      .index = child.index,
      .rotationCandidateIndex = child.rotationCandidateIndex,
      .dobbProxy = child.dobbProxy,
      .isLeaf = child.isLeaf,
    };
  };

  auto childLeafCount = [&](const PackedCwbvhChild &child)
  {
    if (child.isLeaf)
    {
      return 1u;
    }
    return child.index < bvh4NodesCPU_.size() ? bvh4NodesCPU_[child.index].subtreeLeafCount : 0u;
  };

  std::vector<PendingNode> pending;
  pending.push_back(
      PendingNode{
        .bvh4NodeIndex = bvh4NodeIndex,
        .packedNodeIndex = packedNodeIndex,
      });

  for (uint32_t pendingIndex = 0u; pendingIndex < static_cast<uint32_t>(pending.size()); ++pendingIndex)
  {
    const PendingNode pendingNode = pending[pendingIndex];
    if (pendingNode.bvh4NodeIndex >= bvh4NodesCPU_.size())
    {
      continue;
    }

    if (pendingNode.packedNodeIndex >= cwbvhNodesGPU_.size())
    {
      cwbvhNodesGPU_.resize(pendingNode.packedNodeIndex + 1u);
    }
    if (pendingNode.packedNodeIndex >= cwbvhNodeRefsGPU_.size())
    {
      cwbvhNodeRefsGPU_.resize(pendingNode.packedNodeIndex + 1u);
    }

    const BVH4NodeCPU &sourceNode = bvh4NodesCPU_[pendingNode.bvh4NodeIndex];
    std::vector<PackedCwbvhChild> frontier;
    frontier.reserve(8u);

    for (uint32_t childIndex = 0u; childIndex < sourceNode.childCount && childIndex < 4u; ++childIndex)
    {
      frontier.push_back(makeChild(sourceNode.children[childIndex]));
    }

    while (frontier.size() < 8u)
    {
      uint32_t splitIndex = INVALID_NODE;
      float bestCostReduction = -std::numeric_limits<float>::max();
      float bestArea = -1.0f;

      for (uint32_t index = 0u; index < static_cast<uint32_t>(frontier.size()); ++index)
      {
        if (frontier[index].isLeaf)
        {
          continue;
        }

        if (frontier[index].index >= bvh4NodesCPU_.size())
        {
          continue;
        }

        const BVH4NodeCPU &candidate = bvh4NodesCPU_[frontier[index].index];
        if (candidate.childCount < 2u || frontier.size() + candidate.childCount - 1u > 8u)
        {
          continue;
        }

        const float area = aabbSurfaceArea(frontier[index].bounds);
        const float sourceCost = area * static_cast<float>(childLeafCount(frontier[index]));
        float splitCost = 0.0f;
        for (uint32_t childIndex = 0u; childIndex < candidate.childCount && childIndex < 4u; ++childIndex)
        {
          const PackedCwbvhChild splitChild = makeChild(candidate.children[childIndex]);
          splitCost += aabbSurfaceArea(splitChild.bounds) * static_cast<float>(childLeafCount(splitChild));
        }
        const float costReduction = sourceCost - splitCost;
        if (splitIndex == INVALID_NODE || costReduction > bestCostReduction || (costReduction == bestCostReduction && area > bestArea))
        {
          splitIndex = index;
          bestCostReduction = costReduction;
          bestArea = area;
        }
      }

      if (splitIndex == INVALID_NODE)
      {
        break;
      }

      const PackedCwbvhChild childToSplit = frontier[splitIndex];
      if (childToSplit.index >= bvh4NodesCPU_.size())
      {
        break;
      }

      const BVH4NodeCPU &splitNode = bvh4NodesCPU_[childToSplit.index];
      frontier.erase(frontier.begin() + splitIndex);
      for (uint32_t childIndex = 0u; childIndex < splitNode.childCount && childIndex < 4u; ++childIndex)
      {
        frontier.insert(frontier.begin() + splitIndex + childIndex, makeChild(splitNode.children[childIndex]));
      }
    }

    {
      const float parentCenter[3] = {
        (sourceNode.bounds.minPoint[0] + sourceNode.bounds.maxPoint[0]) * 0.5f,
        (sourceNode.bounds.minPoint[1] + sourceNode.bounds.maxPoint[1]) * 0.5f,
        (sourceNode.bounds.minPoint[2] + sourceNode.bounds.maxPoint[2]) * 0.5f,
      };

      float cost[8][8]{};
      for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
      {
        const virtualgeometry::AABB &childBounds = frontier[childIndex].bounds;
        const float childCenter[3] = {
          (childBounds.minPoint[0] + childBounds.maxPoint[0]) * 0.5f,
          (childBounds.minPoint[1] + childBounds.maxPoint[1]) * 0.5f,
          (childBounds.minPoint[2] + childBounds.maxPoint[2]) * 0.5f,
        };

        for (uint32_t slot = 0u; slot < 8u; ++slot)
        {
          const float direction[3] = {
            (slot & 0b100u) != 0u ? -1.0f : 1.0f,
            (slot & 0b010u) != 0u ? -1.0f : 1.0f,
            (slot & 0b001u) != 0u ? -1.0f : 1.0f,
          };
          cost[childIndex][slot] = (childCenter[0] - parentCenter[0]) * direction[0] + (childCenter[1] - parentCenter[1]) * direction[1] + (childCenter[2] - parentCenter[2]) * direction[2];
        }
      }

      int assignment[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
      bool slotFilled[8]{};
      while (true)
      {
        float bestCost = std::numeric_limits<float>::infinity();
        int bestChild = -1;
        int bestSlot = -1;

        for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
        {
          if (assignment[childIndex] != -1)
          {
            continue;
          }

          for (uint32_t slot = 0u; slot < 8u; ++slot)
          {
            if (!slotFilled[slot] && cost[childIndex][slot] < bestCost)
            {
              bestCost = cost[childIndex][slot];
              bestChild = static_cast<int>(childIndex);
              bestSlot = static_cast<int>(slot);
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

      std::vector<PackedCwbvhChild> ordered(8u);
      for (uint32_t childIndex = 0u; childIndex < static_cast<uint32_t>(frontier.size()) && childIndex < 8u; ++childIndex)
      {
        if (assignment[childIndex] >= 0)
        {
          ordered[static_cast<uint32_t>(assignment[childIndex])] = frontier[childIndex];
        }
      }
      frontier = std::move(ordered);
    }

    const uint32_t internalBaseIndex = static_cast<uint32_t>(cwbvhNodesGPU_.size());
    const uint32_t leafBaseIndex = static_cast<uint32_t>(leafInstanceIndicesGPU_.size());

    std::vector<PackedCwbvhChild> internalChildren;
    internalChildren.reserve(frontier.size());

    CWBVHNodeRefGPU nodeRef{};
    for (uint32_t childSlot = 0u; childSlot < static_cast<uint32_t>(frontier.size()) && childSlot < 8u; ++childSlot)
    {
      const PackedCwbvhChild &child = frontier[childSlot];
      if (child.index == INVALID_NODE)
      {
        continue;
      }

      if (!child.isLeaf)
      {
        internalChildren.push_back(child);
      }
      else
      {
        leafInstanceIndicesGPU_.push_back(child.index);
        if (child.index < instanceIndexToLeafNodeIndex_.size())
        {
          nodeRef.childCanonicalIndices[childSlot] = instanceIndexToLeafNodeIndex_[child.index];
        }
      }
    }

    CWBVHNodeGPU node{};
    CWBVHNodeMetadataGPU metadata{};
    encodePackedCwbvhNode(frontier, sourceNode.bounds, internalBaseIndex, leafBaseIndex, sourceNode.depth, false, node, metadata);
    cwbvhNodesGPU_[pendingNode.packedNodeIndex] = node;
    cwbvhNodeRefsGPU_[pendingNode.packedNodeIndex] = nodeRef;
    if (settings_.buildDobbCwbvhNodes)
    {
      if (pendingNode.packedNodeIndex >= dobbCwbvhNodesGPU_.size())
      {
        dobbCwbvhNodesGPU_.resize(pendingNode.packedNodeIndex + 1u);
      }
      if (pendingNode.packedNodeIndex >= cwbvhNodeMetadataGPU_.size())
      {
        cwbvhNodeMetadataGPU_.resize(pendingNode.packedNodeIndex + 1u);
      }
      CWBVHNodeGPU dobbNode{};
      CWBVHNodeMetadataGPU dobbMetadata{};
      encodePackedCwbvhNode(frontier, sourceNode.bounds, internalBaseIndex, leafBaseIndex, sourceNode.depth, true, dobbNode, dobbMetadata);
      dobbCwbvhNodesGPU_[pendingNode.packedNodeIndex] = dobbNode;
      cwbvhNodeMetadataGPU_[pendingNode.packedNodeIndex] = dobbMetadata;
    }

    if (!internalChildren.empty())
    {
      cwbvhNodesGPU_.resize(cwbvhNodesGPU_.size() + internalChildren.size());
      cwbvhNodeRefsGPU_.resize(cwbvhNodeRefsGPU_.size() + internalChildren.size());
      if (settings_.buildDobbCwbvhNodes)
      {
        dobbCwbvhNodesGPU_.resize(dobbCwbvhNodesGPU_.size() + internalChildren.size());
        cwbvhNodeMetadataGPU_.resize(cwbvhNodeMetadataGPU_.size() + internalChildren.size());
      }
      for (uint32_t internalChildIndex = 0u; internalChildIndex < static_cast<uint32_t>(internalChildren.size()); ++internalChildIndex)
      {
        pending.push_back(
            PendingNode{
              .bvh4NodeIndex = internalChildren[internalChildIndex].index,
              .packedNodeIndex = internalBaseIndex + internalChildIndex,
            });
      }
    }
  }
}

SoftwareTLASManager::BVH4Split SoftwareTLASManager::findBvh4Split(std::vector<BuildPrimitive> &primitives, const uint32_t begin, const uint32_t end)
{
  BVH4Split best{};
  best.cost = std::numeric_limits<float>::infinity();
  if (end <= begin + 1u)
  {
    return best;
  }

  float centroidMin[3] = {
    primitives[begin].centroid[0],
    primitives[begin].centroid[1],
    primitives[begin].centroid[2],
  };
  float centroidMax[3] = {
    primitives[begin].centroid[0],
    primitives[begin].centroid[1],
    primitives[begin].centroid[2],
  };

  for (uint32_t index = begin + 1u; index < end; ++index)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      centroidMin[axis] = std::min(centroidMin[axis], primitives[index].centroid[axis]);
      centroidMax[axis] = std::max(centroidMax[axis], primitives[index].centroid[axis]);
    }
  }

  const uint32_t binCount = std::max(2u, settings_.bvh4SahBinCount);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float extent = centroidMax[axis] - centroidMin[axis];
    if (!(extent > 1.0e-8f))
    {
      continue;
    }

    std::vector<BVH4SAHBin> bins(binCount);
    for (uint32_t index = begin; index < end; ++index)
    {
      const float normalized = (primitives[index].centroid[axis] - centroidMin[axis]) / extent;
      const uint32_t binIndex = std::min(binCount - 1u, static_cast<uint32_t>(normalized * static_cast<float>(binCount)));
      BVH4SAHBin &bin = bins[binIndex];
      bin.bounds = bin.count == 0u ? primitives[index].bounds : unionBounds(bin.bounds, primitives[index].bounds);
      ++bin.count;
    }

    std::vector<virtualgeometry::AABB> leftBounds(binCount);
    std::vector<virtualgeometry::AABB> rightBounds(binCount);
    std::vector<uint32_t> leftCounts(binCount, 0u);
    std::vector<uint32_t> rightCounts(binCount, 0u);

    for (uint32_t binIndex = 0u; binIndex < binCount; ++binIndex)
    {
      if (bins[binIndex].count != 0u)
      {
        leftBounds[binIndex] = bins[binIndex].bounds;
        leftCounts[binIndex] = bins[binIndex].count;
      }
      if (binIndex > 0u)
      {
        if (leftCounts[binIndex - 1u] != 0u)
        {
          leftBounds[binIndex] = leftCounts[binIndex] == 0u ? leftBounds[binIndex - 1u] : unionBounds(leftBounds[binIndex - 1u], leftBounds[binIndex]);
        }
        leftCounts[binIndex] += leftCounts[binIndex - 1u];
      }
    }

    for (uint32_t reverseIndex = 0u; reverseIndex < binCount; ++reverseIndex)
    {
      const uint32_t binIndex = binCount - 1u - reverseIndex;
      if (bins[binIndex].count != 0u)
      {
        rightBounds[binIndex] = bins[binIndex].bounds;
        rightCounts[binIndex] = bins[binIndex].count;
      }
      if (binIndex + 1u < binCount)
      {
        if (rightCounts[binIndex + 1u] != 0u)
        {
          rightBounds[binIndex] = rightCounts[binIndex] == 0u ? rightBounds[binIndex + 1u] : unionBounds(rightBounds[binIndex], rightBounds[binIndex + 1u]);
        }
        rightCounts[binIndex] += rightCounts[binIndex + 1u];
      }
    }

    for (uint32_t split = 0u; split + 1u < binCount; ++split)
    {
      if (leftCounts[split] == 0u || rightCounts[split + 1u] == 0u)
      {
        continue;
      }
      const float cost = aabbSurfaceArea(leftBounds[split]) * static_cast<float>(leftCounts[split]) + aabbSurfaceArea(rightBounds[split + 1u]) * static_cast<float>(rightCounts[split + 1u]);
      if (cost < best.cost)
      {
        best.valid = true;
        best.axis = axis;
        best.cost = cost;
        best.mid = begin + leftCounts[split];
      }
    }
  }

  if (!best.valid)
  {
    uint32_t splitAxis = 0u;
    float maxExtent = centroidMax[0] - centroidMin[0];
    for (uint32_t axis = 1u; axis < 3u; ++axis)
    {
      const float extent = centroidMax[axis] - centroidMin[axis];
      if (extent > maxExtent)
      {
        maxExtent = extent;
        splitAxis = axis;
      }
    }
    best.valid = true;
    best.axis = splitAxis;
    best.mid = begin + ((end - begin) >> 1u);
  }

  best.mid = std::clamp(best.mid, begin + 1u, end - 1u);
  std::nth_element(
      primitives.begin() + begin,
      primitives.begin() + best.mid,
      primitives.begin() + end,
      [axis = best.axis](const BuildPrimitive &a, const BuildPrimitive &b)
      {
        return a.centroid[axis] < b.centroid[axis];
      });
  return best;
}

uint32_t SoftwareTLASManager::buildBvh4NodeRecursive(std::vector<BuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const uint32_t depth)
{
  const uint32_t nodeIndex = static_cast<uint32_t>(bvh4NodesCPU_.size());
  bvh4NodesCPU_.push_back(BVH4NodeCPU{});
  BVH4NodeCPU &node = bvh4NodesCPU_[nodeIndex];
  node.depth = depth;
  node.subtreeLeafCount = end - begin;

  if (begin >= end)
  {
    return nodeIndex;
  }

  node.bounds = primitives[begin].bounds;
  for (uint32_t index = begin + 1u; index < end; ++index)
  {
    node.bounds = unionBounds(node.bounds, primitives[index].bounds);
  }

  std::vector<BVH4BuildRange> ranges;
  ranges.push_back(BVH4BuildRange{begin, end});
  while (ranges.size() < 4u)
  {
    uint32_t bestRangeIndex = INVALID_NODE;
    float bestRangeCost = -1.0f;
    BVH4Split bestSplit{};
    for (uint32_t rangeIndex = 0u; rangeIndex < static_cast<uint32_t>(ranges.size()); ++rangeIndex)
    {
      const BVH4BuildRange range = ranges[rangeIndex];
      if (range.end <= range.begin + 1u)
      {
        continue;
      }
      BVH4Split split = findBvh4Split(primitives, range.begin, range.end);
      if (!split.valid)
      {
        continue;
      }
      const float rangeCost = static_cast<float>(range.end - range.begin);
      if (bestRangeIndex == INVALID_NODE || rangeCost > bestRangeCost)
      {
        bestRangeIndex = rangeIndex;
        bestRangeCost = rangeCost;
        bestSplit = split;
      }
    }

    if (bestRangeIndex == INVALID_NODE)
    {
      break;
    }

    const BVH4BuildRange range = ranges[bestRangeIndex];
    ranges.erase(ranges.begin() + bestRangeIndex);
    ranges.insert(ranges.begin() + bestRangeIndex, BVH4BuildRange{bestSplit.mid, range.end});
    ranges.insert(ranges.begin() + bestRangeIndex, BVH4BuildRange{range.begin, bestSplit.mid});
  }

  float largestRotationCandidateChildArea = -1.0f;
  for (const BVH4BuildRange range : ranges)
  {
    if (range.begin >= range.end || node.childCount >= 4u)
    {
      continue;
    }

    BVH4ChildCPU &child = node.children[node.childCount++];
    child.bounds = primitives[range.begin].bounds;
    for (uint32_t index = range.begin + 1u; index < range.end; ++index)
    {
      child.bounds = unionBounds(child.bounds, primitives[index].bounds);
    }

    if (range.end == range.begin + 1u)
    {
      child.isLeaf = true;
      child.index = primitives[range.begin].instanceIndex;
      child.rotationCandidateIndex = primitives[range.begin].rotationCandidateIndex;
      child.dobbProxy = primitives[range.begin].dobbProxy;
    }
    else
    {
      child.isLeaf = false;
      child.index = buildBvh4NodeRecursive(primitives, range.begin, range.end, depth + 1u);
      child.rotationCandidateIndex = child.index < bvh4NodesCPU_.size() ? bvh4NodesCPU_[child.index].rotationCandidateIndex : 0u;
      child.dobbProxy = child.index < bvh4NodesCPU_.size() ? bvh4NodesCPU_[child.index].dobbProxy : emptyDobbProjectionProxy();
    }
    node.dobbProxy = mergeDobbProjectionProxy(node.dobbProxy, child.dobbProxy);

    const float childArea = aabbSurfaceArea(child.bounds);
    if (child.rotationCandidateIndex != 0u && childArea > largestRotationCandidateChildArea)
    {
      largestRotationCandidateChildArea = childArea;
      node.rotationCandidateIndex = child.rotationCandidateIndex;
    }
  }

  return nodeIndex;
}

void SoftwareTLASManager::syncBvh4GpuState()
{
  bvh4NodesGPU_.clear();
  bvh4NodesGPU_.resize(bvh4NodesCPU_.size());
  for (uint32_t nodeIndex = 0u; nodeIndex < static_cast<uint32_t>(bvh4NodesCPU_.size()); ++nodeIndex)
  {
    const BVH4NodeCPU &cpuNode = bvh4NodesCPU_[nodeIndex];
    BVH4NodeGPU gpuNode{};
    for (uint32_t childIndex = 0u; childIndex < cpuNode.childCount && childIndex < 4u; ++childIndex)
    {
      const BVH4ChildCPU &child = cpuNode.children[childIndex];
      gpuNode.childMinX[childIndex] = child.bounds.minPoint[0];
      gpuNode.childMinY[childIndex] = child.bounds.minPoint[1];
      gpuNode.childMinZ[childIndex] = child.bounds.minPoint[2];
      gpuNode.childMaxX[childIndex] = child.bounds.maxPoint[0];
      gpuNode.childMaxY[childIndex] = child.bounds.maxPoint[1];
      gpuNode.childMaxZ[childIndex] = child.bounds.maxPoint[2];
      gpuNode.childIndex[childIndex] = child.index;
      gpuNode.childMeta[childIndex] = bvh4ChildMeta(child.isLeaf);
    }
    bvh4NodesGPU_[nodeIndex] = gpuNode;
  }
}

SoftwareTLASManager::BVH4Stats SoftwareTLASManager::computeBvh4Stats() const
{
  BVH4Stats stats{};
  stats.nodeCount = static_cast<uint32_t>(bvh4NodesCPU_.size());
  if (bvh4NodesCPU_.empty())
  {
    return stats;
  }

  const float rootArea = std::max(aabbSurfaceArea(bvh4NodesCPU_[0u].bounds), 1.0e-6f);
  for (const BVH4NodeCPU &node : bvh4NodesCPU_)
  {
    stats.maxDepth = std::max(stats.maxDepth, node.depth);
    for (uint32_t childIndex = 0u; childIndex < node.childCount && childIndex < 4u; ++childIndex)
    {
      const BVH4ChildCPU &child = node.children[childIndex];
      const uint32_t leafCount = child.isLeaf ? 1u : (child.index < bvh4NodesCPU_.size() ? bvh4NodesCPU_[child.index].subtreeLeafCount : 0u);
      stats.leafCount += child.isLeaf ? 1u : 0u;
      stats.sahScore += (aabbSurfaceArea(child.bounds) / rootArea) * static_cast<float>(std::max(leafCount, 1u));
    }
  }
  return stats;
}

void SoftwareTLASManager::rebuildBvh4State(const bool markAsBuildBaseline)
{
  bvh4NodesCPU_.clear();
  bvh4NodesGPU_.clear();

  std::vector<BuildPrimitive> primitives;
  primitives.reserve(instanceAabbsGPU_.size());
  for (uint32_t instanceIndex = 0u; instanceIndex < static_cast<uint32_t>(instanceAabbsGPU_.size()); ++instanceIndex)
  {
    const InstanceAABBGPU &aabb = instanceAabbsGPU_[instanceIndex];
    if (aabb.instanceIndex == INVALID_INSTANCE_INDEX || aabb.blasHandle == INVALID_BLAS_HANDLE)
    {
      continue;
    }
    BuildPrimitive primitive{};
    primitive.instanceIndex = aabb.instanceIndex;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      primitive.bounds.minPoint[axis] = aabb.minPoint[axis];
      primitive.bounds.maxPoint[axis] = aabb.maxPoint[axis];
    }
    computeCentroid(primitive.bounds, primitive.centroid);
    primitives.push_back(primitive);
  }

  if (!primitives.empty())
  {
    bvh4NodesCPU_.reserve(primitives.size());
    buildBvh4NodeRecursive(primitives, 0u, static_cast<uint32_t>(primitives.size()), 0u);
  }
  syncBvh4GpuState();

  const BVH4Stats stats = computeBvh4Stats();
  lastBuildSummary_.bvh4NodeCount = stats.nodeCount;
  lastBuildSummary_.bvh4LeafCount = stats.leafCount;
  lastBuildSummary_.bvh4MaxDepth = stats.maxDepth;
  lastBuildSummary_.bvh4CurrentSahScore = stats.sahScore;
  if (markAsBuildBaseline)
  {
    lastBuildSummary_.bvh4BuildSahScore = stats.sahScore;
  }
  const float baseline = std::max(lastBuildSummary_.bvh4BuildSahScore, 1.0e-6f);
  lastBuildSummary_.bvh4SahGrowth = stats.sahScore / baseline;
}

bool SoftwareTLASManager::refitBvh4State()
{
  if (bvh4NodesCPU_.empty())
  {
    return false;
  }

  for (uint32_t reverseIndex = 0u; reverseIndex < static_cast<uint32_t>(bvh4NodesCPU_.size()); ++reverseIndex)
  {
    const uint32_t nodeIndex = static_cast<uint32_t>(bvh4NodesCPU_.size()) - 1u - reverseIndex;
    BVH4NodeCPU &node = bvh4NodesCPU_[nodeIndex];
    bool hasBounds = false;
    virtualgeometry::AABB nodeBounds;
    uint32_t subtreeLeafCount = 0u;
    for (uint32_t childIndex = 0u; childIndex < node.childCount && childIndex < 4u; ++childIndex)
    {
      BVH4ChildCPU &child = node.children[childIndex];
      if (child.index == INVALID_NODE)
      {
        continue;
      }
      if (child.isLeaf)
      {
        if (child.index >= instanceAabbsGPU_.size())
        {
          return false;
        }
        const InstanceAABBGPU &aabb = instanceAabbsGPU_[child.index];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          child.bounds.minPoint[axis] = aabb.minPoint[axis];
          child.bounds.maxPoint[axis] = aabb.maxPoint[axis];
        }
        subtreeLeafCount += 1u;
      }
      else
      {
        if (child.index >= bvh4NodesCPU_.size())
        {
          return false;
        }
        child.bounds = bvh4NodesCPU_[child.index].bounds;
        subtreeLeafCount += bvh4NodesCPU_[child.index].subtreeLeafCount;
      }
      nodeBounds = hasBounds ? unionBounds(nodeBounds, child.bounds) : child.bounds;
      hasBounds = true;
    }
    if (hasBounds)
    {
      node.bounds = nodeBounds;
      node.subtreeLeafCount = subtreeLeafCount;
    }
  }

  syncBvh4GpuState();
  const BVH4Stats stats = computeBvh4Stats();
  lastBuildSummary_.bvh4NodeCount = stats.nodeCount;
  lastBuildSummary_.bvh4LeafCount = stats.leafCount;
  lastBuildSummary_.bvh4MaxDepth = stats.maxDepth;
  lastBuildSummary_.bvh4CurrentSahScore = stats.sahScore;
  const float baseline = std::max(lastBuildSummary_.bvh4BuildSahScore, 1.0e-6f);
  lastBuildSummary_.bvh4SahGrowth = stats.sahScore / baseline;
  return true;
}

void SoftwareTLASManager::remapBvh4Instances(const std::vector<uint32_t> &oldInstanceToNewInstance)
{
  for (BVH4NodeCPU &node : bvh4NodesCPU_)
  {
    for (uint32_t childIndex = 0u; childIndex < node.childCount && childIndex < 4u; ++childIndex)
    {
      BVH4ChildCPU &child = node.children[childIndex];
      if (!child.isLeaf || child.index >= oldInstanceToNewInstance.size())
      {
        continue;
      }
      child.index = oldInstanceToNewInstance[child.index];
    }
  }
}

bool SoftwareTLASManager::isValidAabb(const virtualgeometry::AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] && bounds.minPoint[1] <= bounds.maxPoint[1] && bounds.minPoint[2] <= bounds.maxPoint[2];
}

void SoftwareTLASManager::computeCentroid(const virtualgeometry::AABB &bounds, float outCentroid[3])
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outCentroid[axis] = (bounds.minPoint[axis] + bounds.maxPoint[axis]) * 0.5f;
  }
}

virtualgeometry::AABB SoftwareTLASManager::unionBounds(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  return virtualgeometry::AABB::Union(a, b);
}

uint32_t SoftwareTLASManager::buildCanonicalNodeRecursive(std::vector<BuildPrimitive> &primitives, const uint32_t begin, const uint32_t end, const uint32_t parent, const uint32_t depth)
{
  if (begin >= end)
  {
    return INVALID_NODE;
  }

  if (end - begin == 1u)
  {
    CanonicalNodeCPU node{};
    node.parent = parent;
    node.depth = depth;
    node.bounds = primitives[begin].bounds;
    node.instanceIndex = primitives[begin].instanceIndex;
    node.subtreeLeafCount = 1u;
    node.isLeaf = true;
    const uint32_t nodeIndex = static_cast<uint32_t>(canonicalNodesCPU_.size());
    canonicalNodesCPU_.push_back(node);
    if (node.instanceIndex < instanceIndexToLeafNodeIndex_.size())
    {
      instanceIndexToLeafNodeIndex_[node.instanceIndex] = nodeIndex;
    }
    return nodeIndex;
  }

  const BVH4Split split = findBvh4Split(primitives, begin, end);
  const uint32_t mid = split.valid ? split.mid : begin + ((end - begin) >> 1u);

  const uint32_t nodeIndex = static_cast<uint32_t>(canonicalNodesCPU_.size());
  canonicalNodesCPU_.push_back(CanonicalNodeCPU{});
  canonicalNodesCPU_[nodeIndex].parent = parent;
  canonicalNodesCPU_[nodeIndex].depth = depth;
  const uint32_t left = buildCanonicalNodeRecursive(primitives, begin, mid, nodeIndex, depth + 1u);
  const uint32_t right = buildCanonicalNodeRecursive(primitives, mid, end, nodeIndex, depth + 1u);

  CanonicalNodeCPU &node = canonicalNodesCPU_[nodeIndex];
  node.left = left;
  node.right = right;

  if (left != INVALID_NODE)
  {
    node.bounds = canonicalNodesCPU_[left].bounds;
    node.subtreeLeafCount += canonicalNodesCPU_[left].subtreeLeafCount;
  }

  if (right != INVALID_NODE)
  {
    node.bounds = left == INVALID_NODE ? canonicalNodesCPU_[right].bounds : unionBounds(node.bounds, canonicalNodesCPU_[right].bounds);
    node.subtreeLeafCount += canonicalNodesCPU_[right].subtreeLeafCount;
  }

  return nodeIndex;
}

} // namespace softwarebvh
