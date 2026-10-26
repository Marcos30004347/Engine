#include "editor/virtualgeometry/SparseSDFBuilder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace virtualgeometry
{
namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kMaxBruteForceSdfCandidates = 32768u;
constexpr uint32_t kBuildAlgorithmVersion = 24u;

struct Vec3
{
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Triangle
{
  Vec3 a;
  Vec3 b;
  Vec3 c;
};

struct Bounds
{
  Vec3 min;
  Vec3 max;
};

struct TriangleBVHNode
{
  Bounds bounds;
  uint32_t left = UINT32_MAX;
  uint32_t right = UINT32_MAX;
  uint32_t first = 0u;
  uint32_t count = 0u;
};

struct TriangleBVH
{
  std::vector<TriangleBVHNode> nodes;
  std::vector<uint32_t> triangleIndices;
};

struct ParityGrid
{
  Bounds bounds;
  uint32_t resolution = 128u;
  std::vector<std::vector<uint32_t>> bins;
};

struct BrickBuildRecord
{
  Bounds bounds;
  uint32_t level = 0u;
  std::array<std::vector<uint32_t>, sdf::VSDF_BRICK_CELL_COUNT> cellTriangleCandidates;
};

struct RefinementCandidate
{
  uint32_t cell = 0u;
  uint32_t overlapCount = 0u;
};

struct ClosedVoxelMask
{
  std::vector<uint8_t> solidMask;
  uint32_t surfaceCells = 0u;
  uint32_t closedSurfaceCells = 0u;
  uint32_t syntheticClosureCells = 0u;
  uint32_t exteriorCells = 0u;
};

struct BuildContext
{
  std::vector<Triangle> triangles;
  std::vector<Bounds> triangleBounds;
  std::vector<Vec3> triangleCentroids;
  std::vector<Vec3> triangleNormals;
  float triangleNormalSign = 1.0f;
  TriangleBVH bvh;
  ParityGrid parityGrid;
  SparseSDFBuildSettings settings;
  float requestedErrorWorld = 0.0f;
  float rootVolume = 1.0f;
  std::vector<sdf::VSDFNodeDisk> nodes;
  std::vector<sdf::VSDFBrickDisk> bricks;
  std::vector<BrickBuildRecord> brickRecords;
  std::vector<uint8_t> distances;
  std::vector<uint32_t> depthBrickCounts;
  uint32_t maxDepthBuilt = 0u;
  uint32_t sourceShapeIndex = 0u;
  uint32_t outputInstanceIndex = 0u;
  uint32_t totalSourceShapes = 0u;
};

Vec3 makeVec3(const Vertex &vertex)
{
  return Vec3{vertex.pos[0], vertex.pos[1], vertex.pos[2]};
}

Vec3 operator+(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
Vec3 operator/(Vec3 a, float s) { return Vec3{a.x / s, a.y / s, a.z / s}; }

float dot(Vec3 a, Vec3 b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b)
{
  return Vec3{
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x};
}

float lengthSquared(Vec3 v)
{
  return dot(v, v);
}

float length(Vec3 v)
{
  return std::sqrt(lengthSquared(v));
}

Vec3 normalize(Vec3 v)
{
  const float len = length(v);
  if (len <= 1.0e-12f)
    return Vec3{0.0f, 1.0f, 0.0f};
  return v / len;
}

Vec3 minVec(Vec3 a, Vec3 b)
{
  return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vec3 maxVec(Vec3 a, Vec3 b)
{
  return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

float maxExtent(const Bounds &bounds)
{
  const Vec3 extent = bounds.max - bounds.min;
  return std::max(extent.x, std::max(extent.y, extent.z));
}

float diagonalLength(const Bounds &bounds)
{
  return length(bounds.max - bounds.min);
}

float boundsVolume(const Bounds &bounds)
{
  const Vec3 extent = bounds.max - bounds.min;
  return std::max(extent.x * extent.y * extent.z, 1.0e-18f);
}

Bounds makeEmptyBounds()
{
  const float inf = std::numeric_limits<float>::infinity();
  return Bounds{Vec3{inf, inf, inf}, Vec3{-inf, -inf, -inf}};
}

void includePoint(Bounds &bounds, Vec3 point)
{
  bounds.min = minVec(bounds.min, point);
  bounds.max = maxVec(bounds.max, point);
}

Bounds triangleBounds(const Triangle &triangle)
{
  Bounds bounds = makeEmptyBounds();
  includePoint(bounds, triangle.a);
  includePoint(bounds, triangle.b);
  includePoint(bounds, triangle.c);
  return bounds;
}

bool boundsOverlap(const Bounds &a, const Bounds &b)
{
  return a.min.x <= b.max.x && a.max.x >= b.min.x &&
         a.min.y <= b.max.y && a.max.y >= b.min.y &&
         a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool boundsOverlapConservative(const Bounds &a, const Bounds &b, float epsilon)
{
  return a.min.x <= b.max.x + epsilon && a.max.x + epsilon >= b.min.x &&
         a.min.y <= b.max.y + epsilon && a.max.y + epsilon >= b.min.y &&
         a.min.z <= b.max.z + epsilon && a.max.z + epsilon >= b.min.z;
}

float getAxisComponent(Vec3 v, uint32_t axis)
{
  if (axis == 0u)
    return v.x;
  if (axis == 1u)
    return v.y;
  return v.z;
}

bool axisSeparatesTriangleBox(Vec3 axis, Vec3 v0, Vec3 v1, Vec3 v2, Vec3 halfExtent, float epsilon)
{
  const float axisLengthSquared = lengthSquared(axis);
  if (axisLengthSquared <= 1.0e-20f)
    return false;

  const float p0 = dot(v0, axis);
  const float p1 = dot(v1, axis);
  const float p2 = dot(v2, axis);
  const float minProjection = std::min(p0, std::min(p1, p2));
  const float maxProjection = std::max(p0, std::max(p1, p2));
  const float radius =
      halfExtent.x * std::fabs(axis.x) +
      halfExtent.y * std::fabs(axis.y) +
      halfExtent.z * std::fabs(axis.z) +
      epsilon * (std::fabs(axis.x) + std::fabs(axis.y) + std::fabs(axis.z));
  return minProjection > radius || maxProjection < -radius;
}

bool trianglePlaneSeparatesBox(Vec3 normal, Vec3 pointOnPlane, Vec3 halfExtent, float epsilon)
{
  const float projection = dot(normal, pointOnPlane);
  const float radius =
      halfExtent.x * std::fabs(normal.x) +
      halfExtent.y * std::fabs(normal.y) +
      halfExtent.z * std::fabs(normal.z) +
      epsilon * (std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z));
  return projection > radius || projection < -radius;
}

float conservativeOverlapEpsilon(const Bounds &box)
{
  return std::max(maxExtent(box) * 1.0e-4f, 1.0e-7f);
}

bool triangleOverlapsBox(const Triangle &triangle, const Bounds &box, float epsilon)
{
  if (!boundsOverlapConservative(triangleBounds(triangle), box, epsilon))
    return false;

  const Vec3 center = (box.min + box.max) * 0.5f;
  const Vec3 halfExtent = (box.max - box.min) * 0.5f;
  const Vec3 v0 = triangle.a - center;
  const Vec3 v1 = triangle.b - center;
  const Vec3 v2 = triangle.c - center;

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float minProjection = std::min(getAxisComponent(v0, axis), std::min(getAxisComponent(v1, axis), getAxisComponent(v2, axis)));
    const float maxProjection = std::max(getAxisComponent(v0, axis), std::max(getAxisComponent(v1, axis), getAxisComponent(v2, axis)));
    const float radius = getAxisComponent(halfExtent, axis) + epsilon;
    if (minProjection > radius || maxProjection < -radius)
      return false;
  }

  const Vec3 edges[3] = {v1 - v0, v2 - v1, v0 - v2};
  const Vec3 boxAxes[3] = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
  for (const Vec3 &edge : edges)
  {
    for (const Vec3 &boxAxis : boxAxes)
    {
      if (axisSeparatesTriangleBox(cross(edge, boxAxis), v0, v1, v2, halfExtent, epsilon))
        return false;
    }
  }

  const Vec3 normal = cross(v1 - v0, v2 - v0);
  return !trianglePlaneSeparatesBox(normal, v0, halfExtent, epsilon);
}

Bounds expandBounds(const Bounds &bounds, float amount)
{
  return Bounds{
      Vec3{bounds.min.x - amount, bounds.min.y - amount, bounds.min.z - amount},
      Vec3{bounds.max.x + amount, bounds.max.y + amount, bounds.max.z + amount}};
}

Bounds unionBounds(const Bounds &a, const Bounds &b)
{
  return Bounds{minVec(a.min, b.min), maxVec(a.max, b.max)};
}

Bounds expandToCube(Bounds bounds)
{
  Vec3 extent = bounds.max - bounds.min;
  float size = std::max(0.0001f, std::max(extent.x, std::max(extent.y, extent.z)));
  const Vec3 center = (bounds.min + bounds.max) * 0.5f;
  const Vec3 half = Vec3{size, size, size} * 0.5f;
  bounds.min = center - half;
  bounds.max = center + half;
  return bounds;
}

Vec3 closestPointOnTriangle(Vec3 p, const Triangle &tri)
{
  const Vec3 ab = tri.b - tri.a;
  const Vec3 ac = tri.c - tri.a;
  const Vec3 ap = p - tri.a;
  const float d1 = dot(ab, ap);
  const float d2 = dot(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f)
    return tri.a;

  const Vec3 bp = p - tri.b;
  const float d3 = dot(ab, bp);
  const float d4 = dot(ac, bp);
  if (d3 >= 0.0f && d4 <= d3)
    return tri.b;

  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
  {
    const float v = d1 / (d1 - d3);
    return tri.a + ab * v;
  }

  const Vec3 cp = p - tri.c;
  const float d5 = dot(ab, cp);
  const float d6 = dot(ac, cp);
  if (d6 >= 0.0f && d5 <= d6)
    return tri.c;

  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
  {
    const float w = d2 / (d2 - d6);
    return tri.a + ac * w;
  }

  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
  {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return tri.b + (tri.c - tri.b) * w;
  }

  const float denom = 1.0f / (va + vb + vc);
  const float v = vb * denom;
  const float w = vc * denom;
  return tri.a + ab * v + ac * w;
}

float unsignedDistance(Vec3 p, const std::vector<Triangle> &triangles)
{
  float best = std::numeric_limits<float>::infinity();
  for (const Triangle &triangle : triangles)
  {
    const Vec3 closest = closestPointOnTriangle(p, triangle);
    best = std::min(best, length(p - closest));
  }
  return std::isfinite(best) ? best : 0.0f;
}

float distanceSquaredToBounds(Vec3 p, const Bounds &bounds)
{
  const float dx = p.x < bounds.min.x ? bounds.min.x - p.x : (p.x > bounds.max.x ? p.x - bounds.max.x : 0.0f);
  const float dy = p.y < bounds.min.y ? bounds.min.y - p.y : (p.y > bounds.max.y ? p.y - bounds.max.y : 0.0f);
  const float dz = p.z < bounds.min.z ? bounds.min.z - p.z : (p.z > bounds.max.z ? p.z - bounds.max.z : 0.0f);
  return dx * dx + dy * dy + dz * dz;
}

Vec3 triangleCentroid(const Triangle &triangle)
{
  return (triangle.a + triangle.b + triangle.c) / 3.0f;
}

Vec3 triangleNormal(const Triangle &triangle)
{
  return normalize(cross(triangle.b - triangle.a, triangle.c - triangle.a));
}

struct ClosestTriangleQuery
{
  float distanceSquared = std::numeric_limits<float>::infinity();
  Vec3 closestPoint;
  uint32_t triangleIndex = UINT32_MAX;
};

void includeClosestTriangle(ClosestTriangleQuery &query, Vec3 p, const Triangle &triangle, uint32_t triangleIndex)
{
  const Vec3 closest = closestPointOnTriangle(p, triangle);
  const float distanceSquared = lengthSquared(p - closest);
  const float equalDistanceEpsilon = std::max(1.0e-12f, std::max(distanceSquared, query.distanceSquared) * 1.0e-5f);
  if (distanceSquared + equalDistanceEpsilon < query.distanceSquared)
  {
    query.distanceSquared = distanceSquared;
    query.closestPoint = closest;
    query.triangleIndex = triangleIndex;
  }
}

ClosestTriangleQuery closestTriangleQuery(
    Vec3 p,
    const std::vector<Triangle> &triangles,
    const TriangleBVH &bvh,
    float initialBestSquared = std::numeric_limits<float>::infinity())
{
  ClosestTriangleQuery query;
  query.distanceSquared = initialBestSquared;
  if (bvh.nodes.empty())
  {
    for (uint32_t i = 0u; i < static_cast<uint32_t>(triangles.size()); ++i)
      includeClosestTriangle(query, p, triangles[i], i);
    return query;
  }

  thread_local std::vector<uint32_t> stack;
  stack.clear();
  stack.push_back(0u);
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    const TriangleBVHNode &node = bvh.nodes[nodeIndex];
    if (distanceSquaredToBounds(p, node.bounds) > query.distanceSquared)
      continue;

    if (node.count != 0u)
    {
      for (uint32_t i = 0u; i < node.count; ++i)
      {
        const uint32_t triangleIndex = bvh.triangleIndices[node.first + i];
        includeClosestTriangle(query, p, triangles[triangleIndex], triangleIndex);
      }
      continue;
    }

    if (node.left != UINT32_MAX)
      stack.push_back(node.left);
    if (node.right != UINT32_MAX)
      stack.push_back(node.right);
  }

  return query;
}

uint32_t buildBVHNode(TriangleBVH &bvh, const std::vector<Triangle> &triangles, uint32_t first, uint32_t count)
{
  const uint32_t nodeIndex = static_cast<uint32_t>(bvh.nodes.size());
  bvh.nodes.push_back({});

  Bounds bounds = makeEmptyBounds();
  Bounds centroidBounds = makeEmptyBounds();
  for (uint32_t i = 0u; i < count; ++i)
  {
    const Triangle &triangle = triangles[bvh.triangleIndices[first + i]];
    bounds = i == 0u ? triangleBounds(triangle) : unionBounds(bounds, triangleBounds(triangle));
    includePoint(centroidBounds, triangleCentroid(triangle));
  }

  TriangleBVHNode &node = bvh.nodes[nodeIndex];
  node.bounds = bounds;
  node.first = first;
  node.count = count;

  if (count <= 4u)
    return nodeIndex;

  const Vec3 centroidExtent = centroidBounds.max - centroidBounds.min;
  uint32_t axis = 0u;
  if (centroidExtent.y > centroidExtent.x && centroidExtent.y >= centroidExtent.z)
    axis = 1u;
  else if (centroidExtent.z > centroidExtent.x && centroidExtent.z >= centroidExtent.y)
    axis = 2u;

  const uint32_t mid = first + count / 2u;
  std::nth_element(
      bvh.triangleIndices.begin() + first,
      bvh.triangleIndices.begin() + mid,
      bvh.triangleIndices.begin() + first + count,
      [&](uint32_t leftIndex, uint32_t rightIndex) {
        const Vec3 left = triangleCentroid(triangles[leftIndex]);
        const Vec3 right = triangleCentroid(triangles[rightIndex]);
        if (axis == 0u)
          return left.x < right.x;
        if (axis == 1u)
          return left.y < right.y;
        return left.z < right.z;
      });

  node.left = buildBVHNode(bvh, triangles, first, mid - first);
  node.right = buildBVHNode(bvh, triangles, mid, first + count - mid);
  node.count = 0u;
  return nodeIndex;
}

TriangleBVH buildTriangleBVH(const std::vector<Triangle> &triangles)
{
  TriangleBVH bvh;
  bvh.triangleIndices.resize(triangles.size());
  for (uint32_t i = 0u; i < static_cast<uint32_t>(triangles.size()); ++i)
    bvh.triangleIndices[i] = i;
  if (!triangles.empty())
    buildBVHNode(bvh, triangles, 0u, static_cast<uint32_t>(triangles.size()));
  return bvh;
}

float unsignedDistance(Vec3 p, const std::vector<Triangle> &triangles, const TriangleBVH &bvh)
{
  if (bvh.nodes.empty())
    return unsignedDistance(p, triangles);

  float bestSquared = std::numeric_limits<float>::infinity();
  thread_local std::vector<uint32_t> stack;
  stack.clear();
  stack.push_back(0u);
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    const TriangleBVHNode &node = bvh.nodes[nodeIndex];
    if (distanceSquaredToBounds(p, node.bounds) > bestSquared)
      continue;

    if (node.count != 0u)
    {
      for (uint32_t i = 0u; i < node.count; ++i)
      {
        const Triangle &triangle = triangles[bvh.triangleIndices[node.first + i]];
        const Vec3 closest = closestPointOnTriangle(p, triangle);
        bestSquared = std::min(bestSquared, lengthSquared(p - closest));
      }
      continue;
    }

    if (node.left != UINT32_MAX)
      stack.push_back(node.left);
    if (node.right != UINT32_MAX)
      stack.push_back(node.right);
  }

  return std::isfinite(bestSquared) ? std::sqrt(bestSquared) : 0.0f;
}

float solidAngle(Vec3 p, const Triangle &tri)
{
  const Vec3 a = tri.a - p;
  const Vec3 b = tri.b - p;
  const Vec3 c = tri.c - p;
  const float la = length(a);
  const float lb = length(b);
  const float lc = length(c);
  if (la <= 1.0e-8f || lb <= 1.0e-8f || lc <= 1.0e-8f)
    return 0.0f;

  const float numerator = dot(a, cross(b, c));
  const float denominator = la * lb * lc + dot(a, b) * lc + dot(b, c) * la + dot(c, a) * lb;
  return 2.0f * std::atan2(numerator, denominator);
}

float windingNumber(Vec3 p, const std::vector<Triangle> &triangles)
{
  double sum = 0.0;
  for (const Triangle &triangle : triangles)
    sum += static_cast<double>(solidAngle(p, triangle));
  return static_cast<float>(sum / (4.0 * static_cast<double>(kPi)));
}

bool intersectRayBounds(Vec3 origin, Vec3 direction, const Bounds &bounds, float maxT)
{
  float tMin = 0.0f;
  float tMax = maxT;
  const float originArray[3] = {origin.x, origin.y, origin.z};
  const float directionArray[3] = {direction.x, direction.y, direction.z};
  const float minArray[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
  const float maxArray[3] = {bounds.max.x, bounds.max.y, bounds.max.z};

  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (std::fabs(directionArray[axis]) < 1.0e-8f)
    {
      if (originArray[axis] < minArray[axis] || originArray[axis] > maxArray[axis])
        return false;
      continue;
    }

    const float invDirection = 1.0f / directionArray[axis];
    float t0 = (minArray[axis] - originArray[axis]) * invDirection;
    float t1 = (maxArray[axis] - originArray[axis]) * invDirection;
    if (t0 > t1)
      std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMax < tMin)
      return false;
  }

  return true;
}

bool intersectRayTriangle(Vec3 origin, Vec3 direction, const Triangle &triangle, float &outT)
{
  const Vec3 edge1 = triangle.b - triangle.a;
  const Vec3 edge2 = triangle.c - triangle.a;
  const Vec3 pvec = cross(direction, edge2);
  const float det = dot(edge1, pvec);
  if (std::fabs(det) < 1.0e-8f)
    return false;

  const float invDet = 1.0f / det;
  const Vec3 tvec = origin - triangle.a;
  const float u = dot(tvec, pvec) * invDet;
  if (u < -1.0e-6f || u > 1.0f + 1.0e-6f)
    return false;

  const Vec3 qvec = cross(tvec, edge1);
  const float v = dot(direction, qvec) * invDet;
  if (v < -1.0e-6f || u + v > 1.0f + 1.0e-6f)
    return false;

  const float t = dot(edge2, qvec) * invDet;
  if (t <= 1.0e-6f)
    return false;

  outT = t;
  return true;
}

bool rayParityInside(Vec3 p, const std::vector<Triangle> &triangles, const TriangleBVH &bvh)
{
  if (bvh.nodes.empty())
    return false;

  const Vec3 direction = Vec3{1.0f, 0.37139067f, 0.1132277f};
  thread_local std::vector<float> hitDistances;
  hitDistances.clear();
  thread_local std::vector<uint32_t> stack;
  stack.clear();
  stack.push_back(0u);
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    const TriangleBVHNode &node = bvh.nodes[nodeIndex];
    if (!intersectRayBounds(p, direction, node.bounds, std::numeric_limits<float>::infinity()))
      continue;

    if (node.count != 0u)
    {
      for (uint32_t i = 0u; i < node.count; ++i)
      {
        float t = 0.0f;
        if (intersectRayTriangle(p, direction, triangles[bvh.triangleIndices[node.first + i]], t))
          hitDistances.push_back(t);
      }
      continue;
    }

    if (node.left != UINT32_MAX)
      stack.push_back(node.left);
    if (node.right != UINT32_MAX)
      stack.push_back(node.right);
  }

  if (hitDistances.empty())
    return false;

  std::sort(hitDistances.begin(), hitDistances.end());
  uint32_t uniqueHits = 0u;
  float previousT = -std::numeric_limits<float>::infinity();
  for (float t : hitDistances)
  {
    const float epsilon = std::max(1.0e-5f, std::fabs(t) * 1.0e-5f);
    if (uniqueHits == 0u || std::fabs(t - previousT) > epsilon)
    {
      ++uniqueHits;
      previousT = t;
    }
  }
  return (uniqueHits & 1u) != 0u;
}

uint32_t parityGridCell(float value, float minValue, float maxValue, uint32_t resolution)
{
  const float extent = std::max(maxValue - minValue, 1.0e-8f);
  const float normalized = std::clamp((value - minValue) / extent, 0.0f, 0.999999f);
  return std::min<uint32_t>(resolution - 1u, static_cast<uint32_t>(normalized * static_cast<float>(resolution)));
}

void buildParityGrid(BuildContext &context, const Bounds &bounds)
{
  ParityGrid grid;
  grid.bounds = bounds;
  const Vec3 extent = bounds.max - bounds.min;
  const float padding = std::max({extent.x, extent.y, extent.z, 1.0e-6f}) * 1.0e-4f;
  grid.bounds.min.y -= padding;
  grid.bounds.min.z -= padding;
  grid.bounds.max.y += padding;
  grid.bounds.max.z += padding;
  grid.bins.resize(static_cast<size_t>(grid.resolution) * grid.resolution);

  for (uint32_t triangleIndex = 0u; triangleIndex < static_cast<uint32_t>(context.triangles.size()); ++triangleIndex)
  {
    const Bounds &triangleBound = context.triangleBounds[triangleIndex];
    const uint32_t y0 = parityGridCell(triangleBound.min.y, grid.bounds.min.y, grid.bounds.max.y, grid.resolution);
    const uint32_t y1 = parityGridCell(triangleBound.max.y, grid.bounds.min.y, grid.bounds.max.y, grid.resolution);
    const uint32_t z0 = parityGridCell(triangleBound.min.z, grid.bounds.min.z, grid.bounds.max.z, grid.resolution);
    const uint32_t z1 = parityGridCell(triangleBound.max.z, grid.bounds.min.z, grid.bounds.max.z, grid.resolution);
    for (uint32_t z = z0; z <= z1; ++z)
    {
      for (uint32_t y = y0; y <= y1; ++y)
        grid.bins[static_cast<size_t>(z) * grid.resolution + y].push_back(triangleIndex);
    }
  }

  context.parityGrid = std::move(grid);
}

bool parityGridInside(Vec3 p, const BuildContext &context)
{
  const ParityGrid &grid = context.parityGrid;
  if (grid.bins.empty() ||
      p.y < grid.bounds.min.y || p.y > grid.bounds.max.y ||
      p.z < grid.bounds.min.z || p.z > grid.bounds.max.z)
  {
    return rayParityInside(p, context.triangles, context.bvh);
  }

  const uint32_t y = parityGridCell(p.y, grid.bounds.min.y, grid.bounds.max.y, grid.resolution);
  const uint32_t z = parityGridCell(p.z, grid.bounds.min.z, grid.bounds.max.z, grid.resolution);
  const std::vector<uint32_t> &triangles = grid.bins[static_cast<size_t>(z) * grid.resolution + y];
  if (triangles.empty())
    return false;

  const Vec3 direction{1.0f, 0.0f, 0.0f};
  thread_local std::vector<float> hitDistances;
  hitDistances.clear();
  for (uint32_t triangleIndex : triangles)
  {
    float t = 0.0f;
    if (intersectRayTriangle(p, direction, context.triangles[triangleIndex], t))
      hitDistances.push_back(t);
  }

  if (hitDistances.empty())
    return false;

  std::sort(hitDistances.begin(), hitDistances.end());
  uint32_t uniqueHits = 0u;
  float previousT = -std::numeric_limits<float>::infinity();
  for (float t : hitDistances)
  {
    const float epsilon = std::max(1.0e-5f, std::fabs(t) * 1.0e-5f);
    if (uniqueHits == 0u || std::fabs(t - previousT) > epsilon)
    {
      ++uniqueHits;
      previousT = t;
    }
  }
  return (uniqueHits & 1u) != 0u;
}

float calibrateTriangleNormalSign(const BuildContext &context, const Bounds &bounds)
{
  if (context.triangles.empty() || context.triangleNormals.empty())
    return 1.0f;

  const float epsilon = std::max(diagonalLength(bounds) * 1.0e-5f, 1.0e-6f);
  const uint32_t sampleCount = std::min<uint32_t>(static_cast<uint32_t>(context.triangles.size()), 256u);
  const uint32_t stride = std::max<uint32_t>(1u, static_cast<uint32_t>(context.triangles.size()) / sampleCount);
  int32_t score = 0;
  for (uint32_t sample = 0u, triangleIndex = 0u; sample < sampleCount && triangleIndex < context.triangles.size(); ++sample, triangleIndex += stride)
  {
    const Vec3 normal = context.triangleNormals[triangleIndex];
    if (lengthSquared(normal) <= 1.0e-12f)
      continue;

    const Vec3 centroid = context.triangleCentroids[triangleIndex];
    const bool normalSideInside = rayParityInside(centroid + normal * epsilon, context.triangles, context.bvh);
    const bool oppositeSideInside = rayParityInside(centroid - normal * epsilon, context.triangles, context.bvh);
    if (!normalSideInside && oppositeSideInside)
      ++score;
    else if (normalSideInside && !oppositeSideInside)
      --score;
  }
  return score < 0 ? -1.0f : 1.0f;
}

float orientedClosestTriangleSign(Vec3 p, const BuildContext &context, uint32_t triangleIndex, Vec3 closestPoint)
{
  if (triangleIndex >= context.triangleNormals.size())
    return 1.0f;
  return dot(p - closestPoint, context.triangleNormals[triangleIndex]) * context.triangleNormalSign;
}

float signedDistance(Vec3 p, const std::vector<Triangle> &triangles, const TriangleBVH &bvh)
{
  const float distance = unsignedDistance(p, triangles, bvh);
  return rayParityInside(p, triangles, bvh) ? -distance : distance;
}

float signedDistanceFast(Vec3 p, const BuildContext &context)
{
  const ClosestTriangleQuery query = closestTriangleQuery(p, context.triangles, context.bvh);
  if (!std::isfinite(query.distanceSquared) || query.triangleIndex == UINT32_MAX)
    return 0.0f;

  const float distance = std::sqrt(query.distanceSquared);
  return orientedClosestTriangleSign(p, context, query.triangleIndex, query.closestPoint) < 0.0f ? -distance : distance;
}

float signedDistanceFast(Vec3 p, const BuildContext &context, const std::vector<uint32_t> &candidateTriangles, float fallbackDistance)
{
  if (candidateTriangles.empty())
    return fallbackDistance;

  const float fallbackDistanceSquared = fallbackDistance * fallbackDistance;
  if (candidateTriangles.size() > kMaxBruteForceSdfCandidates)
  {
    const ClosestTriangleQuery query = closestTriangleQuery(p, context.triangles, context.bvh, fallbackDistanceSquared);
    if (!std::isfinite(query.distanceSquared) || query.triangleIndex == UINT32_MAX)
      return fallbackDistance;

    const float distance = std::sqrt(query.distanceSquared);
    return orientedClosestTriangleSign(p, context, query.triangleIndex, query.closestPoint) < 0.0f ? -distance : distance;
  }

  float bestSquared = fallbackDistanceSquared;
  uint32_t bestTriangle = UINT32_MAX;
  Vec3 bestClosest{};
  for (uint32_t triangleIndex : candidateTriangles)
  {
    if (distanceSquaredToBounds(p, context.triangleBounds[triangleIndex]) > bestSquared)
      continue;

    const Vec3 closest = closestPointOnTriangle(p, context.triangles[triangleIndex]);
    const float distanceSquared = lengthSquared(p - closest);
    const float equalDistanceEpsilon = std::max(1.0e-12f, std::max(distanceSquared, bestSquared) * 1.0e-5f);
    if (distanceSquared + equalDistanceEpsilon < bestSquared)
    {
      bestSquared = distanceSquared;
      bestTriangle = triangleIndex;
      bestClosest = closest;
    }
  }

  if (!std::isfinite(bestSquared) || bestTriangle == UINT32_MAX)
    return fallbackDistance;

  const float distance = std::sqrt(bestSquared);
  return orientedClosestTriangleSign(p, context, bestTriangle, bestClosest) < 0.0f ? -distance : distance;
}

uint8_t encodeSnorm8(float value)
{
  const float clamped = std::max(-1.0f, std::min(1.0f, value));
  const int encoded = static_cast<int>(std::round(clamped * 127.0f));
  return static_cast<uint8_t>(static_cast<int8_t>(std::max(-127, std::min(127, encoded))));
}

float decodeSnorm8(uint8_t value)
{
  const int32_t signedValue = static_cast<int32_t>(static_cast<int8_t>(value));
  return std::max(-1.0f, static_cast<float>(signedValue) / 127.0f);
}

uint32_t packMinMax(float minValue, float maxValue, float band)
{
  const uint32_t minPacked = static_cast<uint32_t>(encodeSnorm8(minValue / band));
  const uint32_t maxPacked = static_cast<uint32_t>(encodeSnorm8(maxValue / band));
  return minPacked | (maxPacked << 8u);
}

uint32_t packTraceCellBounds(const std::vector<uint8_t> &traceMask)
{
  uint32_t minX = sdf::VSDF_BRICK_RESOLUTION;
  uint32_t minY = sdf::VSDF_BRICK_RESOLUTION;
  uint32_t minZ = sdf::VSDF_BRICK_RESOLUTION;
  uint32_t maxX = 0u;
  uint32_t maxY = 0u;
  uint32_t maxZ = 0u;
  bool anyTraceCell = false;
  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
  {
    if (traceMask[cell] == 0u)
      continue;
    const uint32_t x = cell % sdf::VSDF_BRICK_RESOLUTION;
    const uint32_t y = (cell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
    const uint32_t z = cell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
    minX = std::min(minX, x);
    minY = std::min(minY, y);
    minZ = std::min(minZ, z);
    maxX = std::max(maxX, x);
    maxY = std::max(maxY, y);
    maxZ = std::max(maxZ, z);
    anyTraceCell = true;
  }
  if (!anyTraceCell)
    return UINT32_MAX;
  minX = minX > 0u ? minX - 1u : 0u;
  minY = minY > 0u ? minY - 1u : 0u;
  minZ = minZ > 0u ? minZ - 1u : 0u;
  maxX = std::min(maxX + 1u, sdf::VSDF_BRICK_RESOLUTION - 1u);
  maxY = std::min(maxY + 1u, sdf::VSDF_BRICK_RESOLUTION - 1u);
  maxZ = std::min(maxZ + 1u, sdf::VSDF_BRICK_RESOLUTION - 1u);
  return (minX & 7u) |
         ((minY & 7u) << 3u) |
         ((minZ & 7u) << 6u) |
         ((maxX & 7u) << 9u) |
         ((maxY & 7u) << 12u) |
         ((maxZ & 7u) << 15u);
}

Vec3 samplePosition(const Bounds &bounds, uint32_t x, uint32_t y, uint32_t z)
{
  const Vec3 extent = bounds.max - bounds.min;
  return Vec3{
      bounds.min.x + (static_cast<float>(x) + 0.5f) * extent.x / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION),
      bounds.min.y + (static_cast<float>(y) + 0.5f) * extent.y / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION),
      bounds.min.z + (static_cast<float>(z) + 0.5f) * extent.z / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION)};
}

Vec3 distanceSamplePosition(const Bounds &bounds, uint32_t x, uint32_t y, uint32_t z)
{
  const Vec3 extent = bounds.max - bounds.min;
  return Vec3{
      bounds.min.x + static_cast<float>(x) * extent.x / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION),
      bounds.min.y + static_cast<float>(y) * extent.y / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION),
      bounds.min.z + static_cast<float>(z) * extent.z / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION)};
}

void setAxisComponent(Vec3 &v, uint32_t axis, float value)
{
  if (axis == 0u)
    v.x = value;
  else if (axis == 1u)
    v.y = value;
  else
    v.z = value;
}

uint32_t flatten(uint32_t x, uint32_t y, uint32_t z)
{
  return x + y * sdf::VSDF_BRICK_RESOLUTION + z * sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION;
}

uint32_t flattenDistanceSample(uint32_t x, uint32_t y, uint32_t z)
{
  return x + y * sdf::VSDF_BRICK_DISTANCE_RESOLUTION + z * sdf::VSDF_BRICK_DISTANCE_RESOLUTION * sdf::VSDF_BRICK_DISTANCE_RESOLUTION;
}

Bounds cellBounds(const Bounds &bounds, uint32_t cellIndex);
void collectTriangleOverlaps(const Bounds &cell, const BuildContext &context, std::vector<uint32_t> &outTriangleIndices);

uint32_t popcountBefore(const uint32_t words[16], uint32_t bit)
{
  uint32_t count = 0u;
  const uint32_t wordIndex = bit / 32u;
  for (uint32_t i = 0u; i < wordIndex; ++i)
    count += static_cast<uint32_t>(__builtin_popcount(words[i]));
  const uint32_t localBit = bit % 32u;
  if (localBit != 0u)
    count += static_cast<uint32_t>(__builtin_popcount(words[wordIndex] & ((1u << localBit) - 1u)));
  return count;
}

uint32_t traceGridFlatten(uint32_t x, uint32_t y, uint32_t z)
{
  return x + y * sdf::VSDF_TRACE_GRID_RESOLUTION + z * sdf::VSDF_TRACE_GRID_RESOLUTION * sdf::VSDF_TRACE_GRID_RESOLUTION;
}

void markTraceGridBounds(std::vector<uint8_t> &traceGrid, const Bounds &rootBounds, const Bounds &bounds)
{
  constexpr uint32_t kTraceGridPaddingCells = 2u;
  const Vec3 rootExtent = maxVec(rootBounds.max - rootBounds.min, Vec3{1.0e-8f, 1.0e-8f, 1.0e-8f});
  auto toGridMin =
      [&](float value, float minValue, float extent) -> uint32_t
  {
    const float u = std::clamp((value - minValue) / extent, 0.0f, 0.999999f);
    return std::min<uint32_t>(sdf::VSDF_TRACE_GRID_RESOLUTION - 1u, static_cast<uint32_t>(std::floor(u * static_cast<float>(sdf::VSDF_TRACE_GRID_RESOLUTION))));
  };
  auto toGridMax =
      [&](float value, float minValue, float extent) -> uint32_t
  {
    const float u = std::clamp((value - minValue) / extent, 0.0f, 1.0f);
    const float scaled = u * static_cast<float>(sdf::VSDF_TRACE_GRID_RESOLUTION);
    const int32_t index = static_cast<int32_t>(std::ceil(scaled)) - 1;
    return static_cast<uint32_t>(std::clamp(index, 0, static_cast<int32_t>(sdf::VSDF_TRACE_GRID_RESOLUTION - 1u)));
  };

  const uint32_t gridMinX = toGridMin(bounds.min.x, rootBounds.min.x, rootExtent.x);
  const uint32_t gridMinY = toGridMin(bounds.min.y, rootBounds.min.y, rootExtent.y);
  const uint32_t gridMinZ = toGridMin(bounds.min.z, rootBounds.min.z, rootExtent.z);
  const uint32_t minX = gridMinX > kTraceGridPaddingCells ? gridMinX - kTraceGridPaddingCells : 0u;
  const uint32_t minY = gridMinY > kTraceGridPaddingCells ? gridMinY - kTraceGridPaddingCells : 0u;
  const uint32_t minZ = gridMinZ > kTraceGridPaddingCells ? gridMinZ - kTraceGridPaddingCells : 0u;
  const uint32_t maxX = std::min(toGridMax(bounds.max.x, rootBounds.min.x, rootExtent.x) + kTraceGridPaddingCells, sdf::VSDF_TRACE_GRID_RESOLUTION - 1u);
  const uint32_t maxY = std::min(toGridMax(bounds.max.y, rootBounds.min.y, rootExtent.y) + kTraceGridPaddingCells, sdf::VSDF_TRACE_GRID_RESOLUTION - 1u);
  const uint32_t maxZ = std::min(toGridMax(bounds.max.z, rootBounds.min.z, rootExtent.z) + kTraceGridPaddingCells, sdf::VSDF_TRACE_GRID_RESOLUTION - 1u);

  for (uint32_t z = minZ; z <= maxZ; ++z)
    for (uint32_t y = minY; y <= maxY; ++y)
      for (uint32_t x = minX; x <= maxX; ++x)
        traceGrid[traceGridFlatten(x, y, z)] = 255u;
}

void buildTraceGridNode(
    const std::vector<sdf::VSDFNodeDisk> &nodes,
    uint32_t nodeIndex,
    const Bounds &nodeBounds,
    const Bounds &rootBounds,
    std::vector<uint8_t> &traceGrid)
{
  if (nodeIndex >= nodes.size())
    return;
  const sdf::VSDFNodeDisk &node = nodes[nodeIndex];
  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
  {
    const uint32_t bit = 1u << (cell % 32u);
    const uint32_t word = cell / 32u;
    const bool isTraceCell = (node.traceMaskWords[word] & bit) != 0u;
    const bool hasChildCell = node.firstChild != UINT32_MAX && (node.childMaskWords[word] & bit) != 0u;
    if (!isTraceCell && !hasChildCell)
      continue;
    const Bounds childBounds = cellBounds(nodeBounds, cell);
    if (isTraceCell)
      markTraceGridBounds(traceGrid, rootBounds, childBounds);
    if (hasChildCell)
      buildTraceGridNode(nodes, node.firstChild + popcountBefore(node.childMaskWords, cell), childBounds, rootBounds, traceGrid);
  }
}

std::vector<uint8_t> buildTraceGrid(const std::vector<sdf::VSDFNodeDisk> &nodes, const Bounds &rootBounds)
{
  std::vector<uint8_t> traceGrid(sdf::VSDF_TRACE_GRID_CELL_COUNT, 0u);
  if (!nodes.empty())
    buildTraceGridNode(nodes, 0u, rootBounds, rootBounds, traceGrid);
  return traceGrid;
}

uint32_t flattenTraceAabbCoord(uint32_t x, uint32_t y, uint32_t z, uint32_t dimension)
{
  return x + y * dimension + z * dimension * dimension;
}

void writeTraceAabbBounds(
    std::vector<uint32_t> &tree,
    uint32_t wordOffset,
    uint32_t minX,
    uint32_t minY,
    uint32_t minZ,
    uint32_t maxX,
    uint32_t maxY,
    uint32_t maxZ)
{
  tree[wordOffset + 0u] = minX;
  tree[wordOffset + 1u] = minY;
  tree[wordOffset + 2u] = minZ;
  tree[wordOffset + 3u] = maxX;
  tree[wordOffset + 4u] = maxY;
  tree[wordOffset + 5u] = maxZ;
}

void accumulateTraceGridBounds(
    const std::vector<uint8_t> &traceGrid,
    uint32_t beginX,
    uint32_t beginY,
    uint32_t beginZ,
    uint32_t endX,
    uint32_t endY,
    uint32_t endZ,
    uint32_t &minX,
    uint32_t &minY,
    uint32_t &minZ,
    uint32_t &maxX,
    uint32_t &maxY,
    uint32_t &maxZ,
    bool &any)
{
  for (uint32_t z = beginZ; z < endZ; ++z)
  {
    for (uint32_t y = beginY; y < endY; ++y)
    {
      for (uint32_t x = beginX; x < endX; ++x)
      {
        if (traceGrid[traceGridFlatten(x, y, z)] == 0u)
          continue;
        minX = std::min(minX, x * sdf::VSDF_BRICK_RESOLUTION);
        minY = std::min(minY, y * sdf::VSDF_BRICK_RESOLUTION);
        minZ = std::min(minZ, z * sdf::VSDF_BRICK_RESOLUTION);
        maxX = std::max(maxX, (x + 1u) * sdf::VSDF_BRICK_RESOLUTION);
        maxY = std::max(maxY, (y + 1u) * sdf::VSDF_BRICK_RESOLUTION);
        maxZ = std::max(maxZ, (z + 1u) * sdf::VSDF_BRICK_RESOLUTION);
        any = true;
      }
    }
  }
}

std::vector<uint32_t> buildTraceAabbTree(const std::vector<uint8_t> &traceGrid)
{
  std::vector<uint32_t> tree(sdf::VSDF_TRACE_AABB_TREE_WORD_COUNT, 0u);
  for (uint32_t i = 0u; i < sdf::VSDF_TRACE_AABB_BOTTOM_NODE_COUNT; ++i)
    tree[sdf::VSDF_TRACE_AABB_BOTTOM_WORD_OFFSET + i] = 0x00007fffu;

  for (uint32_t bz = 0u; bz < 16u; ++bz)
  {
    for (uint32_t by = 0u; by < 16u; ++by)
    {
      for (uint32_t bx = 0u; bx < 16u; ++bx)
      {
        uint32_t minX = 32u;
        uint32_t minY = 32u;
        uint32_t minZ = 32u;
        uint32_t maxX = 0u;
        uint32_t maxY = 0u;
        uint32_t maxZ = 0u;
        bool any = false;
        for (uint32_t z = 0u; z < 4u; ++z)
        {
          for (uint32_t y = 0u; y < 4u; ++y)
          {
            for (uint32_t x = 0u; x < 4u; ++x)
            {
              const uint32_t gx = bx * 4u + x;
              const uint32_t gy = by * 4u + y;
              const uint32_t gz = bz * 4u + z;
              if (traceGrid[traceGridFlatten(gx, gy, gz)] == 0u)
                continue;
              minX = std::min(minX, x * sdf::VSDF_BRICK_RESOLUTION);
              minY = std::min(minY, y * sdf::VSDF_BRICK_RESOLUTION);
              minZ = std::min(minZ, z * sdf::VSDF_BRICK_RESOLUTION);
              maxX = std::max(maxX, (x + 1u) * sdf::VSDF_BRICK_RESOLUTION - 1u);
              maxY = std::max(maxY, (y + 1u) * sdf::VSDF_BRICK_RESOLUTION - 1u);
              maxZ = std::max(maxZ, (z + 1u) * sdf::VSDF_BRICK_RESOLUTION - 1u);
              any = true;
            }
          }
        }
        if (!any)
          continue;
        const uint32_t minFlat = flattenTraceAabbCoord(minX, minY, minZ, 32u);
        const uint32_t maxFlat = flattenTraceAabbCoord(maxX, maxY, maxZ, 32u);
        tree[sdf::VSDF_TRACE_AABB_BOTTOM_WORD_OFFSET + flattenTraceAabbCoord(bx, by, bz, 16u)] =
            (minFlat & 0x7fffu) | ((maxFlat & 0x7fffu) << 16u);
      }
    }
  }

  for (uint32_t mz = 0u; mz < 4u; ++mz)
  {
    for (uint32_t my = 0u; my < 4u; ++my)
    {
      for (uint32_t mx = 0u; mx < 4u; ++mx)
      {
        uint32_t minX = UINT32_MAX;
        uint32_t minY = UINT32_MAX;
        uint32_t minZ = UINT32_MAX;
        uint32_t maxX = 0u;
        uint32_t maxY = 0u;
        uint32_t maxZ = 0u;
        bool any = false;
        accumulateTraceGridBounds(traceGrid, mx * 16u, my * 16u, mz * 16u, (mx + 1u) * 16u, (my + 1u) * 16u, (mz + 1u) * 16u, minX, minY, minZ, maxX, maxY, maxZ, any);
        if (!any)
          continue;
        const uint32_t wordOffset = sdf::VSDF_TRACE_AABB_MID_WORD_OFFSET + flattenTraceAabbCoord(mx, my, mz, 4u) * 6u;
        writeTraceAabbBounds(tree, wordOffset, minX, minY, minZ, maxX, maxY, maxZ);
      }
    }
  }

  uint32_t minX = UINT32_MAX;
  uint32_t minY = UINT32_MAX;
  uint32_t minZ = UINT32_MAX;
  uint32_t maxX = 0u;
  uint32_t maxY = 0u;
  uint32_t maxZ = 0u;
  bool any = false;
  accumulateTraceGridBounds(traceGrid, 0u, 0u, 0u, sdf::VSDF_TRACE_GRID_RESOLUTION, sdf::VSDF_TRACE_GRID_RESOLUTION, sdf::VSDF_TRACE_GRID_RESOLUTION, minX, minY, minZ, maxX, maxY, maxZ, any);
  if (any)
    writeTraceAabbBounds(tree, sdf::VSDF_TRACE_AABB_ROOT_WORD_OFFSET, minX, minY, minZ, maxX, maxY, maxZ);
  return tree;
}

struct TraceBrickMapBuildResult
{
  std::vector<uint8_t> traceGrid;
  std::vector<uint32_t> tracePageTable;
  std::vector<sdf::VSDFTracePageDisk> tracePages;
  std::vector<uint32_t> tracePageMasks;
  std::vector<uint32_t> traceBrickMap;
};

struct TracePageBuildRecord
{
  uint32_t maskWords[sdf::VSDF_TRACE_PAGE_MASK_WORD_COUNT] = {};
  uint32_t brickIndices[sdf::VSDF_TRACE_PAGE_CELL_COUNT] = {};
  bool occupied = false;
};

uint32_t traceRootFlatten(uint32_t x, uint32_t y, uint32_t z)
{
  return x + y * sdf::VSDF_TRACE_ROOT_RESOLUTION + z * sdf::VSDF_TRACE_ROOT_RESOLUTION * sdf::VSDF_TRACE_ROOT_RESOLUTION;
}

Bounds traceGridCellBounds(const Bounds &rootBounds, uint32_t x, uint32_t y, uint32_t z)
{
  const Vec3 extent = rootBounds.max - rootBounds.min;
  const Vec3 cellSize = extent / static_cast<float>(sdf::VSDF_TRACE_GRID_RESOLUTION);
  const Vec3 minPoint{
      rootBounds.min.x + cellSize.x * static_cast<float>(x),
      rootBounds.min.y + cellSize.y * static_cast<float>(y),
      rootBounds.min.z + cellSize.z * static_cast<float>(z)};
  return Bounds{minPoint, minPoint + cellSize};
}

uint32_t appendTraceBrickSDF(BuildContext &context, const Bounds &bounds, const std::vector<uint32_t> &surfaceTriangles)
{
  const float cellVoxelSize = maxExtent(bounds) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
  const Bounds sampleBounds = expandBounds(bounds, cellVoxelSize);
  const float voxelSize = maxExtent(sampleBounds) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
  const float band = std::max(voxelSize * static_cast<float>(context.settings.narrowBandVoxels), 1.0e-6f);

  std::vector<uint32_t> sdfTriangleCandidates;
  collectTriangleOverlaps(expandBounds(sampleBounds, band), context, sdfTriangleCandidates);
  if (sdfTriangleCandidates.empty())
    sdfTriangleCandidates = surfaceTriangles;

  const uint32_t brickIndex = static_cast<uint32_t>(context.bricks.size());
  const uint32_t distanceOffset = static_cast<uint32_t>(context.distances.size());
  context.bricks.push_back({});
  context.distances.resize(context.distances.size() + sdf::VSDF_BRICK_DISTANCE_STRIDE);

  float minDistance = std::numeric_limits<float>::infinity();
  float maxDistance = -std::numeric_limits<float>::infinity();
  for (uint32_t z = 0u; z < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++z)
  {
    for (uint32_t y = 0u; y < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++y)
    {
      for (uint32_t x = 0u; x < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++x)
      {
        const Vec3 p = distanceSamplePosition(sampleBounds, x, y, z);
        const float d = signedDistanceFast(p, context, sdfTriangleCandidates, band);
        minDistance = std::min(minDistance, d);
        maxDistance = std::max(maxDistance, d);
        context.distances[distanceOffset + flattenDistanceSample(x, y, z)] = encodeSnorm8(d / band);
      }
    }
  }

  if (!std::isfinite(minDistance))
    minDistance = 0.0f;
  if (!std::isfinite(maxDistance))
    maxDistance = 0.0f;

  sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
  brick.distanceOffset = distanceOffset;
  brick.minMaxDistancePacked = packMinMax(minDistance, maxDistance, band);
  brick.voxelSize = voxelSize;
  brick.maxError = 0.0f;
  return brickIndex;
}

TraceBrickMapBuildResult buildTraceBrickMap(BuildContext &context, const Bounds &rootBounds)
{
  TraceBrickMapBuildResult result;
  result.traceGrid.assign(sdf::VSDF_TRACE_GRID_CELL_COUNT, 0u);
  result.tracePageTable.assign(sdf::VSDF_TRACE_ROOT_CELL_COUNT, sdf::VSDF_TRACE_INVALID_BRICK);
  std::array<TracePageBuildRecord, sdf::VSDF_TRACE_ROOT_CELL_COUNT> pageRecords;
  for (TracePageBuildRecord &page : pageRecords)
    for (uint32_t &brickIndex : page.brickIndices)
      brickIndex = sdf::VSDF_TRACE_INVALID_BRICK;

  std::vector<uint32_t> surfaceTriangles;
  const float cellSize = maxExtent(rootBounds) / static_cast<float>(sdf::VSDF_TRACE_GRID_RESOLUTION);
  const float overlapPad = std::max(cellSize / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION), 1.0e-7f);
  for (uint32_t z = 0u; z < sdf::VSDF_TRACE_GRID_RESOLUTION; ++z)
  {
    for (uint32_t y = 0u; y < sdf::VSDF_TRACE_GRID_RESOLUTION; ++y)
    {
      for (uint32_t x = 0u; x < sdf::VSDF_TRACE_GRID_RESOLUTION; ++x)
      {
        const Bounds cell = traceGridCellBounds(rootBounds, x, y, z);
        collectTriangleOverlaps(expandBounds(cell, overlapPad), context, surfaceTriangles);
        if (surfaceTriangles.empty())
          continue;

        const uint32_t flat = traceGridFlatten(x, y, z);
        const uint32_t rootX = x / sdf::VSDF_TRACE_PAGE_RESOLUTION;
        const uint32_t rootY = y / sdf::VSDF_TRACE_PAGE_RESOLUTION;
        const uint32_t rootZ = z / sdf::VSDF_TRACE_PAGE_RESOLUTION;
        const uint32_t localX = x % sdf::VSDF_TRACE_PAGE_RESOLUTION;
        const uint32_t localY = y % sdf::VSDF_TRACE_PAGE_RESOLUTION;
        const uint32_t localZ = z % sdf::VSDF_TRACE_PAGE_RESOLUTION;
        const uint32_t rootFlat = traceRootFlatten(rootX, rootY, rootZ);
        const uint32_t localFlat = localX + localY * sdf::VSDF_TRACE_PAGE_RESOLUTION + localZ * sdf::VSDF_TRACE_PAGE_RESOLUTION * sdf::VSDF_TRACE_PAGE_RESOLUTION;
        TracePageBuildRecord &page = pageRecords[rootFlat];
        result.traceGrid[flat] = 255u;
        page.occupied = true;
        page.maskWords[localFlat / 32u] |= 1u << (localFlat % 32u);
        page.brickIndices[localFlat] = appendTraceBrickSDF(context, cell, surfaceTriangles);
      }
    }
  }

  for (uint32_t rootIndex = 0u; rootIndex < sdf::VSDF_TRACE_ROOT_CELL_COUNT; ++rootIndex)
  {
    const TracePageBuildRecord &pageRecord = pageRecords[rootIndex];
    if (!pageRecord.occupied)
      continue;

    const uint32_t pageIndex = static_cast<uint32_t>(result.tracePages.size());
    result.tracePageTable[rootIndex] = pageIndex;

    sdf::VSDFTracePageDisk page{};
    page.maskOffset = static_cast<uint32_t>(result.tracePageMasks.size());
    page.brickOffset = static_cast<uint32_t>(result.traceBrickMap.size());
    for (uint32_t word : pageRecord.maskWords)
    {
      page.childCount += static_cast<uint32_t>(__builtin_popcount(word));
      result.tracePageMasks.push_back(word);
    }
    result.tracePages.push_back(page);

    for (uint32_t localFlat = 0u; localFlat < sdf::VSDF_TRACE_PAGE_CELL_COUNT; ++localFlat)
    {
      const uint32_t word = localFlat / 32u;
      const uint32_t bit = 1u << (localFlat % 32u);
      if ((pageRecord.maskWords[word] & bit) == 0u)
        continue;
      result.traceBrickMap.push_back(pageRecord.brickIndices[localFlat]);
    }
  }

  return result;
}

template <typename Function>
void parallelForFixedWorkers(size_t count, uint32_t requestedWorkers, Function &&function)
{
  if (count == 0u)
    return;

  const size_t workerCount = std::min<size_t>(
      count,
      std::max<uint32_t>(1u, requestedWorkers));
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

Bounds cellBounds(const Bounds &bounds, uint32_t cellIndex)
{
  const uint32_t x = cellIndex % sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t y = (cellIndex / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t z = cellIndex / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
  const Vec3 extent = (bounds.max - bounds.min) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
  const Vec3 minPoint = bounds.min + Vec3{extent.x * static_cast<float>(x), extent.y * static_cast<float>(y), extent.z * static_cast<float>(z)};
  return Bounds{minPoint, minPoint + extent};
}

ClosedVoxelMask buildClosedVoxelMask(const std::vector<uint8_t> &surfaceMask, uint32_t closeVoxels)
{
  ClosedVoxelMask result;
  std::vector<uint8_t> barrier(sdf::VSDF_BRICK_CELL_COUNT, 0u);

  // Close small gaps before computing volume. This proxy shell is only used for
  // volume/flood-fill decisions; SDF emission still uses the original triangles.
  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
  {
    if (surfaceMask[cell] == 0u)
      continue;

    ++result.surfaceCells;
    const uint32_t x = cell % sdf::VSDF_BRICK_RESOLUTION;
    const uint32_t y = (cell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
    const uint32_t z = cell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);

    const int radius = static_cast<int>(std::min<uint32_t>(closeVoxels, sdf::VSDF_BRICK_RESOLUTION - 1u));
    for (int dz = -radius; dz <= radius; ++dz)
    {
      const int nz = static_cast<int>(z) + dz;
      if (nz < 0 || nz >= static_cast<int>(sdf::VSDF_BRICK_RESOLUTION))
        continue;
      for (int dy = -radius; dy <= radius; ++dy)
      {
        const int ny = static_cast<int>(y) + dy;
        if (ny < 0 || ny >= static_cast<int>(sdf::VSDF_BRICK_RESOLUTION))
          continue;
        for (int dx = -radius; dx <= radius; ++dx)
        {
          const int nx = static_cast<int>(x) + dx;
          if (nx < 0 || nx >= static_cast<int>(sdf::VSDF_BRICK_RESOLUTION))
            continue;
          barrier[flatten(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny), static_cast<uint32_t>(nz))] = 1u;
        }
      }
    }
  }

  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
  {
    if (barrier[cell] == 0u)
      continue;
    ++result.closedSurfaceCells;
    if (surfaceMask[cell] == 0u)
      ++result.syntheticClosureCells;
  }

  std::vector<uint8_t> outside(sdf::VSDF_BRICK_CELL_COUNT, 0u);
  std::vector<uint32_t> stack;
  stack.reserve(sdf::VSDF_BRICK_CELL_COUNT);

  auto pushOutside = [&](uint32_t x, uint32_t y, uint32_t z)
  {
    const uint32_t cell = flatten(x, y, z);
    if (barrier[cell] != 0u || outside[cell] != 0u)
      return;
    outside[cell] = 1u;
    ++result.exteriorCells;
    stack.push_back(cell);
  };

  for (uint32_t z = 0u; z < sdf::VSDF_BRICK_RESOLUTION; ++z)
  {
    for (uint32_t y = 0u; y < sdf::VSDF_BRICK_RESOLUTION; ++y)
    {
      pushOutside(0u, y, z);
      pushOutside(sdf::VSDF_BRICK_RESOLUTION - 1u, y, z);
    }
    for (uint32_t x = 0u; x < sdf::VSDF_BRICK_RESOLUTION; ++x)
    {
      pushOutside(x, 0u, z);
      pushOutside(x, sdf::VSDF_BRICK_RESOLUTION - 1u, z);
    }
  }
  for (uint32_t y = 0u; y < sdf::VSDF_BRICK_RESOLUTION; ++y)
  {
    for (uint32_t x = 0u; x < sdf::VSDF_BRICK_RESOLUTION; ++x)
    {
      pushOutside(x, y, 0u);
      pushOutside(x, y, sdf::VSDF_BRICK_RESOLUTION - 1u);
    }
  }

  while (!stack.empty())
  {
    const uint32_t cell = stack.back();
    stack.pop_back();
    const uint32_t x = cell % sdf::VSDF_BRICK_RESOLUTION;
    const uint32_t y = (cell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
    const uint32_t z = cell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
    if (x > 0u)
      pushOutside(x - 1u, y, z);
    if (x + 1u < sdf::VSDF_BRICK_RESOLUTION)
      pushOutside(x + 1u, y, z);
    if (y > 0u)
      pushOutside(x, y - 1u, z);
    if (y + 1u < sdf::VSDF_BRICK_RESOLUTION)
      pushOutside(x, y + 1u, z);
    if (z > 0u)
      pushOutside(x, y, z - 1u);
    if (z + 1u < sdf::VSDF_BRICK_RESOLUTION)
      pushOutside(x, y, z + 1u);
  }

  result.solidMask.resize(sdf::VSDF_BRICK_CELL_COUNT, 0u);
  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
    result.solidMask[cell] = (barrier[cell] != 0u || outside[cell] == 0u) ? 1u : 0u;
  return result;
}

uint32_t countTriangleOverlaps(const Bounds &cell, const BuildContext &context)
{
  if (context.bvh.nodes.empty())
    return 0u;

  const float overlapEpsilon = conservativeOverlapEpsilon(cell);
  uint32_t overlaps = 0u;
  thread_local std::vector<uint32_t> stack;
  stack.clear();
  stack.push_back(0u);
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    const TriangleBVHNode &node = context.bvh.nodes[nodeIndex];
    if (!boundsOverlapConservative(cell, node.bounds, overlapEpsilon))
      continue;

    if (node.count != 0u)
    {
      for (uint32_t i = 0u; i < node.count; ++i)
      {
        const uint32_t triangleIndex = context.bvh.triangleIndices[node.first + i];
        if (boundsOverlapConservative(cell, context.triangleBounds[triangleIndex], overlapEpsilon) &&
            triangleOverlapsBox(context.triangles[triangleIndex], cell, overlapEpsilon))
          ++overlaps;
      }
      continue;
    }

    if (node.left != UINT32_MAX)
      stack.push_back(node.left);
    if (node.right != UINT32_MAX)
      stack.push_back(node.right);
  }

  return overlaps;
}

void collectTriangleOverlaps(const Bounds &cell, const BuildContext &context, std::vector<uint32_t> &outTriangleIndices)
{
  outTriangleIndices.clear();
  if (context.bvh.nodes.empty())
    return;

  const float overlapEpsilon = conservativeOverlapEpsilon(cell);
  thread_local std::vector<uint32_t> stack;
  stack.clear();
  stack.push_back(0u);
  while (!stack.empty())
  {
    const uint32_t nodeIndex = stack.back();
    stack.pop_back();
    const TriangleBVHNode &node = context.bvh.nodes[nodeIndex];
    if (!boundsOverlapConservative(cell, node.bounds, overlapEpsilon))
      continue;

    if (node.count != 0u)
    {
      for (uint32_t i = 0u; i < node.count; ++i)
      {
        const uint32_t triangleIndex = context.bvh.triangleIndices[node.first + i];
        if (boundsOverlapConservative(cell, context.triangleBounds[triangleIndex], overlapEpsilon) &&
            triangleOverlapsBox(context.triangles[triangleIndex], cell, overlapEpsilon))
          outTriangleIndices.push_back(triangleIndex);
      }
      continue;
    }

    if (node.left != UINT32_MAX)
      stack.push_back(node.left);
    if (node.right != UINT32_MAX)
      stack.push_back(node.right);
  }
}

void collectTriangleOverlapsFromCandidates(
    const Bounds &cell,
    const BuildContext &context,
    const std::vector<uint32_t> &candidateTriangles,
    std::vector<uint32_t> &outTriangleIndices)
{
  outTriangleIndices.clear();
  if (candidateTriangles.empty())
    return;

  if (candidateTriangles.size() > 8192u)
  {
    collectTriangleOverlaps(cell, context, outTriangleIndices);
    return;
  }

  const float overlapEpsilon = conservativeOverlapEpsilon(cell);
  for (uint32_t triangleIndex : candidateTriangles)
  {
    if (boundsOverlapConservative(cell, context.triangleBounds[triangleIndex], overlapEpsilon) &&
        triangleOverlapsBox(context.triangles[triangleIndex], cell, overlapEpsilon))
      outTriangleIndices.push_back(triangleIndex);
  }
}

void sortUniqueTriangleCandidates(std::vector<uint32_t> &candidates)
{
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
}

bool isBrickBoundaryCell(uint32_t cell)
{
  const uint32_t x = cell % sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t y = (cell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
  const uint32_t z = cell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
  return x == 0u || y == 0u || z == 0u ||
         x + 1u == sdf::VSDF_BRICK_RESOLUTION ||
         y + 1u == sdf::VSDF_BRICK_RESOLUTION ||
         z + 1u == sdf::VSDF_BRICK_RESOLUTION;
}

void collectEmissionTriangleCandidates(
    const Bounds &cellBoundsValue,
    const BuildContext &context,
    const std::vector<uint32_t> &candidateTriangles,
    bool includeExternalNeighbors,
    std::vector<uint32_t> &outTriangleIndices)
{
  const Bounds queryBounds = expandBounds(cellBoundsValue, maxExtent(cellBoundsValue));
  collectTriangleOverlapsFromCandidates(queryBounds, context, candidateTriangles, outTriangleIndices);
  if (includeExternalNeighbors)
  {
    std::vector<uint32_t> externalCandidates;
    collectTriangleOverlaps(queryBounds, context, externalCandidates);
    outTriangleIndices.insert(outTriangleIndices.end(), externalCandidates.begin(), externalCandidates.end());
  }
  sortUniqueTriangleCandidates(outTriangleIndices);
}

void logProgress(const BuildContext &context, const std::string &message)
{
  if (!context.settings.progressLogging)
    return;
  std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
            << " instance " << context.outputInstanceIndex
            << " depth " << context.maxDepthBuilt
            << " bricks " << context.bricks.size()
            << " nodes " << context.nodes.size()
            << ": " << message << '\n';
}

void collectSurfaceNeighborhoodCandidates(
    const BrickBuildRecord &record,
    uint32_t x,
    uint32_t y,
    uint32_t z,
    std::vector<uint32_t> &outCandidates)
{
  outCandidates.clear();
  for (int dz = -1; dz <= 1; ++dz)
  {
    const int nz = static_cast<int>(z) + dz;
    if (nz < 0 || nz >= static_cast<int>(sdf::VSDF_BRICK_RESOLUTION))
      continue;
    for (int dy = -1; dy <= 1; ++dy)
    {
      const int ny = static_cast<int>(y) + dy;
      if (ny < 0 || ny >= static_cast<int>(sdf::VSDF_BRICK_RESOLUTION))
        continue;
      for (int dx = -1; dx <= 1; ++dx)
      {
        const int nx = static_cast<int>(x) + dx;
        if (nx < 0 || nx >= static_cast<int>(sdf::VSDF_BRICK_RESOLUTION))
          continue;
        const std::vector<uint32_t> &cellCandidates =
            record.cellTriangleCandidates[flatten(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny), static_cast<uint32_t>(nz))];
        outCandidates.insert(outCandidates.end(), cellCandidates.begin(), cellCandidates.end());
      }
    }
  }
  sortUniqueTriangleCandidates(outCandidates);
}

void mergeTriangleCandidates(std::vector<uint32_t> &candidates, const std::vector<uint32_t> &extraCandidates)
{
  if (extraCandidates.empty())
    return;
  candidates.insert(candidates.end(), extraCandidates.begin(), extraCandidates.end());
  sortUniqueTriangleCandidates(candidates);
}

void emitBrickSDF(BuildContext &context, uint32_t brickIndex)
{
  const BrickBuildRecord &record = context.brickRecords[brickIndex];
  const Bounds &bounds = record.bounds;
  const float voxelSize = maxExtent(bounds) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
  const float band = std::max(voxelSize * static_cast<float>(context.settings.narrowBandVoxels), 1.0e-6f);
  std::vector<uint32_t> sdfTriangleCandidates;
  collectTriangleOverlaps(expandBounds(bounds, band + voxelSize * 2.0f), context, sdfTriangleCandidates);

  const uint32_t distanceOffset = context.bricks[brickIndex].distanceOffset;
  float minDistance = std::numeric_limits<float>::infinity();
  float maxDistance = -std::numeric_limits<float>::infinity();
  std::vector<uint32_t> localTriangleCandidates;

  for (uint32_t z = 0u; z < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++z)
  {
    for (uint32_t y = 0u; y < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++y)
    {
      for (uint32_t x = 0u; x < sdf::VSDF_BRICK_DISTANCE_RESOLUTION; ++x)
      {
        const Vec3 p = distanceSamplePosition(bounds, x, y, z);
        collectSurfaceNeighborhoodCandidates(record, x, y, z, localTriangleCandidates);
        mergeTriangleCandidates(localTriangleCandidates, sdfTriangleCandidates);
        const std::vector<uint32_t> &distanceCandidates = localTriangleCandidates.empty() ? sdfTriangleCandidates : localTriangleCandidates;
        const float d = signedDistanceFast(p, context, distanceCandidates, band);
        minDistance = std::min(minDistance, d);
        maxDistance = std::max(maxDistance, d);
        context.distances[distanceOffset + flattenDistanceSample(x, y, z)] = encodeSnorm8(d / band);
      }
    }
  }

  if (!std::isfinite(minDistance))
    minDistance = 0.0f;
  if (!std::isfinite(maxDistance))
    maxDistance = 0.0f;

  sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
  brick.minMaxDistancePacked = packMinMax(minDistance, maxDistance, band);
  brick.voxelSize = voxelSize;
}

float brickBand(const BuildContext &context, uint32_t brickIndex)
{
  return std::max(
      context.bricks[brickIndex].voxelSize * static_cast<float>(context.settings.narrowBandVoxels),
      1.0e-6f);
}

float decodeBrickDistance(const BuildContext &context, uint32_t brickIndex, uint32_t flatCell)
{
  const sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
  const uint32_t offset = brick.distanceOffset + flatCell;
  if (offset >= context.distances.size())
    return 0.0f;
  return decodeSnorm8(context.distances[offset]) * brickBand(context, brickIndex);
}

uint32_t axisMinCell(uint32_t axis, uint32_t side, uint32_t u, uint32_t v)
{
  uint32_t coord[3] = {0u, 0u, 0u};
  coord[axis] = side == 0u ? 0u : sdf::VSDF_BRICK_RESOLUTION - 1u;
  const uint32_t axisU = (axis + 1u) % 3u;
  const uint32_t axisV = (axis + 2u) % 3u;
  coord[axisU] = u;
  coord[axisV] = v;
  return flatten(coord[0], coord[1], coord[2]);
}

uint32_t nearestCellCoord(float value, float minValue, float maxValue)
{
  const float extent = std::max(maxValue - minValue, 1.0e-8f);
  const float normalized = std::clamp((value - minValue) / extent, 0.0f, 0.999999f);
  return std::min<uint32_t>(
      sdf::VSDF_BRICK_RESOLUTION - 1u,
      static_cast<uint32_t>(normalized * static_cast<float>(sdf::VSDF_BRICK_RESOLUTION)));
}

uint32_t nearestCellInBrick(const Bounds &bounds, Vec3 p)
{
  return flatten(
      nearestCellCoord(p.x, bounds.min.x, bounds.max.x),
      nearestCellCoord(p.y, bounds.min.y, bounds.max.y),
      nearestCellCoord(p.z, bounds.min.z, bounds.max.z));
}

uint32_t nearestCellOnFace(const Bounds &bounds, uint32_t axis, uint32_t side, Vec3 p)
{
  uint32_t coord[3] = {
      nearestCellCoord(p.x, bounds.min.x, bounds.max.x),
      nearestCellCoord(p.y, bounds.min.y, bounds.max.y),
      nearestCellCoord(p.z, bounds.min.z, bounds.max.z)};
  coord[axis] = side == 0u ? 0u : sdf::VSDF_BRICK_RESOLUTION - 1u;
  return flatten(coord[0], coord[1], coord[2]);
}

bool intervalOverlaps(float aMin, float aMax, float bMin, float bMax, float epsilon)
{
  return aMin <= bMax + epsilon && aMax + epsilon >= bMin;
}

bool projectedBoundsOverlap(const Bounds &a, const Bounds &b, uint32_t axis, float epsilon)
{
  for (uint32_t otherAxis = 0u; otherAxis < 3u; ++otherAxis)
  {
    if (otherAxis == axis)
      continue;
    if (!intervalOverlaps(
            getAxisComponent(a.min, otherAxis),
            getAxisComponent(a.max, otherAxis),
            getAxisComponent(b.min, otherAxis),
            getAxisComponent(b.max, otherAxis),
            epsilon))
    {
      return false;
    }
  }
  return true;
}

bool projectedPointInside(const Bounds &bounds, uint32_t axis, Vec3 p, float epsilon)
{
  for (uint32_t otherAxis = 0u; otherAxis < 3u; ++otherAxis)
  {
    if (otherAxis == axis)
      continue;
    const float value = getAxisComponent(p, otherAxis);
    if (value < getAxisComponent(bounds.min, otherAxis) - epsilon ||
        value > getAxisComponent(bounds.max, otherAxis) + epsilon)
    {
      return false;
    }
  }
  return true;
}

bool boundsContainsBounds(const Bounds &outer, const Bounds &inner, float epsilon)
{
  return inner.min.x >= outer.min.x - epsilon && inner.max.x <= outer.max.x + epsilon &&
         inner.min.y >= outer.min.y - epsilon && inner.max.y <= outer.max.y + epsilon &&
         inner.min.z >= outer.min.z - epsilon && inner.max.z <= outer.max.z + epsilon;
}

void addNeighborDistance(
    const BuildContext &context,
    uint32_t sourceBrick,
    uint32_t sourceCell,
    uint32_t targetBrick,
    uint32_t targetCell,
    std::vector<float> &distanceSums,
    std::vector<uint32_t> &distanceWeights)
{
  const uint32_t sourceOffset = context.bricks[sourceBrick].distanceOffset + sourceCell;
  if (sourceOffset >= distanceSums.size())
    return;
  distanceSums[sourceOffset] += decodeBrickDistance(context, targetBrick, targetCell);
  distanceWeights[sourceOffset] += 1u;
}

void writeBrickDistance(BuildContext &context, uint32_t brickIndex, uint32_t flatCell, float distance)
{
  const sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
  const uint32_t offset = brick.distanceOffset + flatCell;
  if (offset >= context.distances.size())
    return;
  context.distances[offset] = encodeSnorm8(distance / brickBand(context, brickIndex));
}

void stitchFaceToFace(
    const BuildContext &context,
    uint32_t sourceBrick,
    uint32_t sourceAxis,
    uint32_t sourceSide,
    uint32_t targetBrick,
    uint32_t targetSide,
    float epsilon,
    std::vector<float> &distanceSums,
    std::vector<uint32_t> &distanceWeights)
{
  const Bounds &sourceBounds = context.brickRecords[sourceBrick].bounds;
  const Bounds &targetBounds = context.brickRecords[targetBrick].bounds;
  if (!projectedBoundsOverlap(sourceBounds, targetBounds, sourceAxis, epsilon))
    return;

  for (uint32_t v = 0u; v < sdf::VSDF_BRICK_RESOLUTION; ++v)
  {
    for (uint32_t u = 0u; u < sdf::VSDF_BRICK_RESOLUTION; ++u)
    {
      const uint32_t sourceCell = axisMinCell(sourceAxis, sourceSide, u, v);
      const uint32_t sx = sourceCell % sdf::VSDF_BRICK_RESOLUTION;
      const uint32_t sy = (sourceCell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
      const uint32_t sz = sourceCell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
      Vec3 p = samplePosition(sourceBounds, sx, sy, sz);
      if (!projectedPointInside(targetBounds, sourceAxis, p, epsilon))
        continue;
      setAxisComponent(p, sourceAxis, getAxisComponent(targetSide == 0u ? targetBounds.min : targetBounds.max, sourceAxis));
      const uint32_t targetCell = nearestCellOnFace(targetBounds, sourceAxis, targetSide, p);
      addNeighborDistance(context, sourceBrick, sourceCell, targetBrick, targetCell, distanceSums, distanceWeights);
    }
  }
}

void forceFaceToFaceAgreement(
    BuildContext &context,
    uint32_t brickA,
    uint32_t axis,
    uint32_t sideA,
    uint32_t brickB,
    uint32_t sideB,
    float epsilon)
{
  const Bounds &boundsA = context.brickRecords[brickA].bounds;
  const Bounds &boundsB = context.brickRecords[brickB].bounds;
  if (!projectedBoundsOverlap(boundsA, boundsB, axis, epsilon))
    return;

  for (uint32_t v = 0u; v < sdf::VSDF_BRICK_RESOLUTION; ++v)
  {
    for (uint32_t u = 0u; u < sdf::VSDF_BRICK_RESOLUTION; ++u)
    {
      const uint32_t cellA = axisMinCell(axis, sideA, u, v);
      const uint32_t ax = cellA % sdf::VSDF_BRICK_RESOLUTION;
      const uint32_t ay = (cellA / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
      const uint32_t az = cellA / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
      Vec3 p = samplePosition(boundsA, ax, ay, az);
      if (!projectedPointInside(boundsB, axis, p, epsilon))
        continue;
      setAxisComponent(p, axis, getAxisComponent(sideB == 0u ? boundsB.min : boundsB.max, axis));
      const uint32_t cellB = nearestCellOnFace(boundsB, axis, sideB, p);
      const float average = (decodeBrickDistance(context, brickA, cellA) + decodeBrickDistance(context, brickB, cellB)) * 0.5f;
      writeBrickDistance(context, brickA, cellA, average);
      writeBrickDistance(context, brickB, cellB, average);
    }
  }
}

void stitchFaceToContainingBrick(
    const BuildContext &context,
    uint32_t sourceBrick,
    uint32_t sourceAxis,
    uint32_t sourceSide,
    uint32_t targetBrick,
    float epsilon,
    std::vector<float> &distanceSums,
    std::vector<uint32_t> &distanceWeights)
{
  const Bounds &sourceBounds = context.brickRecords[sourceBrick].bounds;
  const Bounds &targetBounds = context.brickRecords[targetBrick].bounds;
  for (uint32_t v = 0u; v < sdf::VSDF_BRICK_RESOLUTION; ++v)
  {
    for (uint32_t u = 0u; u < sdf::VSDF_BRICK_RESOLUTION; ++u)
    {
      const uint32_t sourceCell = axisMinCell(sourceAxis, sourceSide, u, v);
      const uint32_t sx = sourceCell % sdf::VSDF_BRICK_RESOLUTION;
      const uint32_t sy = (sourceCell / sdf::VSDF_BRICK_RESOLUTION) % sdf::VSDF_BRICK_RESOLUTION;
      const uint32_t sz = sourceCell / (sdf::VSDF_BRICK_RESOLUTION * sdf::VSDF_BRICK_RESOLUTION);
      Vec3 p = samplePosition(sourceBounds, sx, sy, sz);
      setAxisComponent(
          p,
          sourceAxis,
          sourceSide == 0u ? getAxisComponent(sourceBounds.min, sourceAxis) : getAxisComponent(sourceBounds.max, sourceAxis));
      if (!boundsContainsBounds(targetBounds, Bounds{p, p}, epsilon))
        continue;
      const uint32_t targetCell = nearestCellInBrick(targetBounds, p);
      addNeighborDistance(context, sourceBrick, sourceCell, targetBrick, targetCell, distanceSums, distanceWeights);
      addNeighborDistance(context, targetBrick, targetCell, sourceBrick, sourceCell, distanceSums, distanceWeights);
    }
  }
}

void stitchBrickBoundarySDFs(BuildContext &context)
{
  if (context.bricks.size() < 2u || context.distances.empty())
    return;

  std::vector<float> distanceSums(context.distances.size(), 0.0f);
  std::vector<uint32_t> distanceWeights(context.distances.size(), 1u);
  for (uint32_t brickIndex = 0u; brickIndex < static_cast<uint32_t>(context.bricks.size()); ++brickIndex)
  {
    const sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
    for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
      distanceSums[brick.distanceOffset + cell] = decodeBrickDistance(context, brickIndex, cell);
  }

  for (uint32_t a = 0u; a < static_cast<uint32_t>(context.bricks.size()); ++a)
  {
    for (uint32_t b = a + 1u; b < static_cast<uint32_t>(context.bricks.size()); ++b)
    {
      const Bounds &boundsA = context.brickRecords[a].bounds;
      const Bounds &boundsB = context.brickRecords[b].bounds;
      const float epsilon = std::max({context.bricks[a].voxelSize, context.bricks[b].voxelSize, 1.0e-6f}) * 1.0e-4f;

      if (boundsContainsBounds(boundsA, boundsB, epsilon))
      {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          stitchFaceToContainingBrick(context, b, axis, 0u, a, epsilon, distanceSums, distanceWeights);
          stitchFaceToContainingBrick(context, b, axis, 1u, a, epsilon, distanceSums, distanceWeights);
        }
      }
      else if (boundsContainsBounds(boundsB, boundsA, epsilon))
      {
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          stitchFaceToContainingBrick(context, a, axis, 0u, b, epsilon, distanceSums, distanceWeights);
          stitchFaceToContainingBrick(context, a, axis, 1u, b, epsilon, distanceSums, distanceWeights);
        }
      }

      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        if (std::fabs(getAxisComponent(boundsA.max, axis) - getAxisComponent(boundsB.min, axis)) <= epsilon)
        {
          stitchFaceToFace(context, a, axis, 1u, b, 0u, epsilon, distanceSums, distanceWeights);
          stitchFaceToFace(context, b, axis, 0u, a, 1u, epsilon, distanceSums, distanceWeights);
        }
        if (std::fabs(getAxisComponent(boundsB.max, axis) - getAxisComponent(boundsA.min, axis)) <= epsilon)
        {
          stitchFaceToFace(context, b, axis, 1u, a, 0u, epsilon, distanceSums, distanceWeights);
          stitchFaceToFace(context, a, axis, 0u, b, 1u, epsilon, distanceSums, distanceWeights);
        }
      }
    }
  }

  for (uint32_t brickIndex = 0u; brickIndex < static_cast<uint32_t>(context.bricks.size()); ++brickIndex)
  {
    sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
    const float band = brickBand(context, brickIndex);
    float minDistance = std::numeric_limits<float>::infinity();
    float maxDistance = -std::numeric_limits<float>::infinity();
    for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
    {
      const uint32_t offset = brick.distanceOffset + cell;
      const float distance = distanceSums[offset] / static_cast<float>(std::max(distanceWeights[offset], 1u));
      minDistance = std::min(minDistance, distance);
      maxDistance = std::max(maxDistance, distance);
      context.distances[offset] = encodeSnorm8(distance / band);
    }
    brick.minMaxDistancePacked = packMinMax(minDistance, maxDistance, band);
  }

  for (uint32_t a = 0u; a < static_cast<uint32_t>(context.bricks.size()); ++a)
  {
    for (uint32_t b = a + 1u; b < static_cast<uint32_t>(context.bricks.size()); ++b)
    {
      const Bounds &boundsA = context.brickRecords[a].bounds;
      const Bounds &boundsB = context.brickRecords[b].bounds;
      const float epsilon = std::max({context.bricks[a].voxelSize, context.bricks[b].voxelSize, 1.0e-6f}) * 1.0e-4f;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        if (std::fabs(getAxisComponent(boundsA.max, axis) - getAxisComponent(boundsB.min, axis)) <= epsilon)
          forceFaceToFaceAgreement(context, a, axis, 1u, b, 0u, epsilon);
        if (std::fabs(getAxisComponent(boundsB.max, axis) - getAxisComponent(boundsA.min, axis)) <= epsilon)
          forceFaceToFaceAgreement(context, b, axis, 1u, a, 0u, epsilon);
      }
    }
  }

  for (uint32_t brickIndex = 0u; brickIndex < static_cast<uint32_t>(context.bricks.size()); ++brickIndex)
  {
    sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
    const float band = brickBand(context, brickIndex);
    float minDistance = std::numeric_limits<float>::infinity();
    float maxDistance = -std::numeric_limits<float>::infinity();
    for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
    {
      const float distance = decodeBrickDistance(context, brickIndex, cell);
      minDistance = std::min(minDistance, distance);
      maxDistance = std::max(maxDistance, distance);
    }
    brick.minMaxDistancePacked = packMinMax(minDistance, maxDistance, band);
  }
}

void emitAllBrickSDFs(BuildContext &context)
{
  if (context.settings.progressLogging)
  {
    std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
              << " instance " << context.outputInstanceIndex
              << " sdf emission begin bricks=" << context.bricks.size()
              << " workers=" << context.settings.sdfWorkerCount
              << '\n';
  }

  std::atomic<uint32_t> completed{0u};
  std::mutex progressLogMutex;
  parallelForFixedWorkers(
      context.bricks.size(),
      context.settings.sdfWorkerCount,
      [&](size_t brickIndex)
      {
        emitBrickSDF(context, static_cast<uint32_t>(brickIndex));
        const uint32_t done = completed.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (context.settings.progressLogging &&
            (done == context.bricks.size() ||
             done == 1u ||
             (context.settings.progressLogBrickInterval != 0u && (done % context.settings.progressLogBrickInterval) == 0u)))
        {
          const BrickBuildRecord &record = context.brickRecords[brickIndex];
          std::lock_guard<std::mutex> lock(progressLogMutex);
          std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
                    << " instance " << context.outputInstanceIndex
                    << " sdf progress completed=" << done << "/" << context.bricks.size()
                    << " currentDepth=" << record.level
                    << " allocated=" << context.bricks.size()
                    << '\n';
        }
      });
}

void buildNodeAt(
    BuildContext &context,
    const Bounds &bounds,
    uint32_t level,
    uint32_t nodeIndex,
    uint32_t guardLevelsRemaining,
    const std::vector<uint32_t> &candidateTriangles)
{
  const uint32_t brickIndex = static_cast<uint32_t>(context.bricks.size());
  context.bricks.push_back({});
  context.brickRecords.push_back(BrickBuildRecord{bounds, level});
  if (context.depthBrickCounts.size() <= level)
    context.depthBrickCounts.resize(level + 1u, 0u);
  context.depthBrickCounts[level] += 1u;
  context.maxDepthBuilt = std::max(context.maxDepthBuilt, level);

  const bool logThisBrick =
      context.settings.progressLogging &&
      (brickIndex == 0u ||
       context.settings.progressLogBrickInterval == 0u ||
       (brickIndex % context.settings.progressLogBrickInterval) == 0u);

  const uint32_t distanceOffset = static_cast<uint32_t>(context.distances.size());
  context.distances.resize(context.distances.size() + sdf::VSDF_BRICK_DISTANCE_STRIDE);

  sdf::VSDFBrickDisk &brick = context.bricks[brickIndex];
  brick.distanceOffset = distanceOffset;
  brick.minMaxDistancePacked = 0u;
  brick.voxelSize = maxExtent(bounds) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
  brick.maxError = 0.0f;

  sdf::VSDFNodeDisk &node = context.nodes[nodeIndex];
  node.brickIndex = brickIndex;
  node.firstChild = UINT32_MAX;
  node.level = static_cast<uint16_t>(level);
  node.flags = 0u;
  node._padding0 = UINT32_MAX;
  for (uint32_t &word : node.childMaskWords)
    word = 0u;
  for (uint32_t &word : node.validMaskWords)
    word = 0u;
  for (uint32_t &word : node.traceMaskWords)
    word = 0u;

  if (logThisBrick)
  {
    std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
              << " instance " << context.outputInstanceIndex
              << " topology brick " << brickIndex
              << " depth " << level
              << " allocated=" << context.bricks.size()
              << " voxelSize=" << brick.voxelSize
              << " triangles=" << context.triangles.size()
              << '\n';
  }

  uint32_t surfaceOverlapCount = 0u;
  float voxelizedMeshVolume = 0.0f;
  float maxChildVolumeRatio = 0.0f;
  std::vector<uint32_t> refinedCells;
  std::vector<RefinementCandidate> occupiedCells;
  std::vector<std::vector<uint32_t>> childTriangleCandidates;
  std::vector<uint8_t> surfaceMask(sdf::VSDF_BRICK_CELL_COUNT, 0u);
  std::vector<uint8_t> traceMask(sdf::VSDF_BRICK_CELL_COUNT, 0u);
  uint32_t overlappingCells = 0u;
  uint32_t enclosedCells = 0u;
  uint32_t closedSurfaceCells = 0u;
  uint32_t syntheticClosureCells = 0u;
  uint32_t exteriorCells = 0u;
  bool usedGuardRefinement = false;
  const float currentVoxelVolume = boundsVolume(bounds);
  const float targetVolumeRatio = std::max(context.settings.requestedError, 1.0e-12f);
  const float volumeSlack = context.rootVolume * std::max(
                                           context.settings.requestedError * context.settings.requestedError * context.settings.requestedError,
                                           1.0e-12f);
  const float currentVoxelVolumeRatio = currentVoxelVolume / context.rootVolume;

  if (level < context.settings.maxLevel)
  {
    childTriangleCandidates.resize(sdf::VSDF_BRICK_CELL_COUNT);
    for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
    {
      const Bounds childBounds = cellBounds(bounds, cell);
      collectTriangleOverlapsFromCandidates(childBounds, context, candidateTriangles, childTriangleCandidates[cell]);
      const uint32_t childOverlapCount = static_cast<uint32_t>(childTriangleCandidates[cell].size());
      collectEmissionTriangleCandidates(
          childBounds,
          context,
          candidateTriangles,
          childOverlapCount != 0u && isBrickBoundaryCell(cell),
          context.brickRecords[brickIndex].cellTriangleCandidates[cell]);
      surfaceOverlapCount += childOverlapCount;
      if (childOverlapCount == 0u)
        continue;

      ++overlappingCells;
      occupiedCells.push_back(RefinementCandidate{cell, childOverlapCount});
      surfaceMask[cell] = 1u;
      traceMask[cell] = 1u;
    }

    const ClosedVoxelMask closedMask = buildClosedVoxelMask(surfaceMask, context.settings.surfaceCloseVoxels);
    closedSurfaceCells = closedMask.closedSurfaceCells;
    syntheticClosureCells = closedMask.syntheticClosureCells;
    exteriorCells = closedMask.exteriorCells;
    for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
    {
      if (closedMask.solidMask[cell] == 0u)
        continue;
      ++enclosedCells;
      const Bounds childBounds = cellBounds(bounds, cell);
      const float childVolume = boundsVolume(childBounds);
      voxelizedMeshVolume += childVolume;
      maxChildVolumeRatio = std::max(maxChildVolumeRatio, childVolume / currentVoxelVolume);
    }

    const float localMeshToVoxelRatio = voxelizedMeshVolume / (currentVoxelVolume + volumeSlack);
    if (localMeshToVoxelRatio > targetVolumeRatio)
    {
      std::sort(
          occupiedCells.begin(),
          occupiedCells.end(),
          [](const RefinementCandidate &left, const RefinementCandidate &right)
          {
            if (left.overlapCount != right.overlapCount)
              return left.overlapCount > right.overlapCount;
            return left.cell < right.cell;
          });
      const size_t selectedCount = std::min<size_t>(occupiedCells.size(), context.settings.maxRefinedCellsPerBrick);
      refinedCells.reserve(selectedCount);
      for (size_t i = 0u; i < selectedCount; ++i)
        refinedCells.push_back(occupiedCells[i].cell);
      std::sort(refinedCells.begin(), refinedCells.end());
    }

    if (refinedCells.empty() && guardLevelsRemaining != 0u && !occupiedCells.empty())
    {
      const size_t selectedCount = std::min<size_t>(occupiedCells.size(), context.settings.maxRefinedCellsPerBrick);
      refinedCells.reserve(selectedCount);
      for (size_t i = 0u; i < selectedCount; ++i)
        refinedCells.push_back(occupiedCells[i].cell);
      std::sort(refinedCells.begin(), refinedCells.end());
      usedGuardRefinement = true;
    }

    for (uint32_t cell : refinedCells)
      context.nodes[nodeIndex].childMaskWords[cell / 32u] |= 1u << (cell % 32u);
  }
  else
  {
    std::vector<uint32_t> cellTriangleCandidates;
    for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
    {
      cellTriangleCandidates.clear();
      const Bounds childBounds = cellBounds(bounds, cell);
      collectTriangleOverlapsFromCandidates(childBounds, context, candidateTriangles, cellTriangleCandidates);
      const uint32_t childOverlapCount = static_cast<uint32_t>(cellTriangleCandidates.size());
      collectEmissionTriangleCandidates(
          childBounds,
          context,
          candidateTriangles,
          childOverlapCount != 0u && isBrickBoundaryCell(cell),
          context.brickRecords[brickIndex].cellTriangleCandidates[cell]);
      surfaceOverlapCount += childOverlapCount;
      if (childOverlapCount == 0u)
        continue;
      surfaceMask[cell] = 1u;
      traceMask[cell] = 1u;
      ++overlappingCells;
    }
    enclosedCells = overlappingCells;
    closedSurfaceCells = overlappingCells;
    const float childVolume = currentVoxelVolume / static_cast<float>(sdf::VSDF_BRICK_CELL_COUNT);
    voxelizedMeshVolume = childVolume * static_cast<float>(overlappingCells);
    maxChildVolumeRatio = overlappingCells != 0u ? childVolume / currentVoxelVolume : 0.0f;
  }

  for (uint32_t cell = 0u; cell < sdf::VSDF_BRICK_CELL_COUNT; ++cell)
  {
    if (surfaceMask[cell] != 0u)
      context.nodes[nodeIndex].validMaskWords[cell / 32u] |= 1u << (cell % 32u);
    if (traceMask[cell] != 0u)
      context.nodes[nodeIndex].traceMaskWords[cell / 32u] |= 1u << (cell % 32u);
  }
  context.nodes[nodeIndex]._padding0 = packTraceCellBounds(traceMask);

  const float meshToVoxelRatio = std::min(voxelizedMeshVolume / (currentVoxelVolume + volumeSlack), 1.0f);
  brick.maxError = meshToVoxelRatio;

  const bool hasErrorToRefine = meshToVoxelRatio > targetVolumeRatio;
  const bool canRefine = level < context.settings.maxLevel && !refinedCells.empty();
  if (logThisBrick)
  {
    std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
              << " instance " << context.outputInstanceIndex
              << " depth " << level
              << " brick " << brickIndex
              << " meshToVoxelRatio=" << meshToVoxelRatio
              << " targetVolumeRatio=" << targetVolumeRatio
              << " voxelizedMeshVolume=" << voxelizedMeshVolume
              << " currentVoxelVolume=" << currentVoxelVolume
              << " volumeSlack=" << volumeSlack
              << " currentVoxelVolumeRatio=" << currentVoxelVolumeRatio
              << " maxChildVolumeRatio=" << maxChildVolumeRatio
              << " requestedError=" << context.settings.requestedError
              << " surfaceOverlaps=" << surfaceOverlapCount
              << " overlappingCells=" << overlappingCells
              << " enclosedCells=" << enclosedCells
              << " closedSurfaceCells=" << closedSurfaceCells
              << " syntheticClosureCells=" << syntheticClosureCells
              << " exteriorCells=" << exteriorCells
              << " surfaceCloseVoxels=" << context.settings.surfaceCloseVoxels
              << " maxRefinedCellsPerBrick=" << context.settings.maxRefinedCellsPerBrick
              << " allocated=" << context.bricks.size()
              << '\n';
    if (canRefine)
    {
      std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
                << " instance " << context.outputInstanceIndex
                << " depth " << level
                << " brick " << brickIndex
                << (usedGuardRefinement ? " selecting guard child cells for refinement" : " selecting child cells for refinement")
                << " allocated=" << context.bricks.size()
                << '\n';
    }
    else
    {
      std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
                << " instance " << context.outputInstanceIndex
                << " depth " << level
                << " brick " << brickIndex
                << " not refining reason="
                << (level >= context.settings.maxLevel ? "max-level" : (surfaceOverlapCount == 0u ? "no-overlapping-surface" : (hasErrorToRefine ? "no-refinable-occupied-cells" : "within-volume-ratio")))
                << '\n';
    }
  }

  if (canRefine)
  {
    if (logThisBrick)
    {
      std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
                << " instance " << context.outputInstanceIndex
                << " depth " << level
                << " brick " << brickIndex
                << " refinement candidates overlappingCells=" << overlappingCells
                << " selectedCells=" << refinedCells.size()
                << " meshToVoxelRatio=" << meshToVoxelRatio
                << " maxChildVolumeRatio=" << maxChildVolumeRatio
                << " guard=" << (usedGuardRefinement ? "true" : "false")
                << " allocated=" << context.bricks.size()
                << '\n';
    }

    if (!refinedCells.empty())
    {
      const uint32_t firstChild = static_cast<uint32_t>(context.nodes.size());
      context.nodes[nodeIndex].firstChild = firstChild;
      context.nodes.resize(context.nodes.size() + refinedCells.size());
      for (size_t childOrdinal = 0u; childOrdinal < refinedCells.size(); ++childOrdinal)
      {
        const Bounds childBounds = cellBounds(bounds, refinedCells[childOrdinal]);
        buildNodeAt(
            context,
            childBounds,
            level + 1u,
            firstChild + static_cast<uint32_t>(childOrdinal),
            usedGuardRefinement && guardLevelsRemaining != 0u ? guardLevelsRemaining - 1u : guardLevelsRemaining,
            childTriangleCandidates[refinedCells[childOrdinal]]);
      }
    }
  }

  if (logThisBrick)
  {
    std::cerr << "[VSDF] shape " << (context.sourceShapeIndex + 1u) << "/" << context.totalSourceShapes
              << " instance " << context.outputInstanceIndex
              << " finished brick " << brickIndex
              << " depth " << level
              << " allocated=" << context.bricks.size()
              << '\n';
  }
}

uint32_t buildNode(BuildContext &context, const Bounds &bounds, uint32_t level)
{
  const uint32_t nodeIndex = static_cast<uint32_t>(context.nodes.size());
  context.nodes.push_back({});
  std::vector<uint32_t> rootCandidates(context.triangles.size());
  for (uint32_t i = 0u; i < static_cast<uint32_t>(rootCandidates.size()); ++i)
    rootCandidates[i] = i;
  buildNodeAt(context, bounds, level, nodeIndex, context.settings.guardLeafLevels, rootCandidates);

  return nodeIndex;
}

std::vector<Triangle> collectShapeTriangles(const VirtualGeometrySourceAsset &asset, const SourceShapeBuildInput &shape)
{
  std::vector<Triangle> triangles;
  for (const MeshPart &meshPart : shape.meshParts)
  {
    const std::vector<uint32_t> &indices = meshPart.shape.indices;
    for (size_t i = 0u; i + 2u < indices.size(); i += 3u)
    {
      const uint32_t i0 = indices[i + 0u];
      const uint32_t i1 = indices[i + 1u];
      const uint32_t i2 = indices[i + 2u];
      if (i0 >= asset.vertices.size() || i1 >= asset.vertices.size() || i2 >= asset.vertices.size())
        continue;
      triangles.push_back(Triangle{makeVec3(asset.vertices[i0]), makeVec3(asset.vertices[i1]), makeVec3(asset.vertices[i2])});
    }
  }
  return triangles;
}

void cacheTriangleMetadata(BuildContext &context)
{
  context.triangleBounds.resize(context.triangles.size());
  context.triangleCentroids.resize(context.triangles.size());
  context.triangleNormals.resize(context.triangles.size());
  for (size_t i = 0u; i < context.triangles.size(); ++i)
  {
    context.triangleBounds[i] = triangleBounds(context.triangles[i]);
    context.triangleCentroids[i] = triangleCentroid(context.triangles[i]);
    context.triangleNormals[i] = triangleNormal(context.triangles[i]);
  }
}

Bounds computeBounds(const std::vector<Triangle> &triangles)
{
  Bounds bounds = makeEmptyBounds();
  for (const Triangle &triangle : triangles)
  {
    includePoint(bounds, triangle.a);
    includePoint(bounds, triangle.b);
    includePoint(bounds, triangle.c);
  }
  if (!std::isfinite(bounds.min.x))
    bounds = Bounds{Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}};
  return expandToCube(bounds);
}

uint64_t hashBytes(uint64_t hash, const void *data, size_t size)
{
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0u; i < size; ++i)
  {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t hashAsset(const VirtualGeometrySourceAsset &asset)
{
  uint64_t hash = 1469598103934665603ull;
  for (const Vertex &vertex : asset.vertices)
    hash = hashBytes(hash, vertex.pos, sizeof(vertex.pos));
  for (const SourceShapeBuildInput &shape : asset.shapes)
  {
    for (const MeshPart &part : shape.meshParts)
    {
      if (!part.shape.indices.empty())
        hash = hashBytes(hash, part.shape.indices.data(), part.shape.indices.size() * sizeof(uint32_t));
    }
  }
  return hash;
}

uint64_t hashSettings(const SparseSDFBuildSettings &settings)
{
  uint64_t hash = 1469598103934665603ull;
  hash = hashBytes(hash, &kBuildAlgorithmVersion, sizeof(kBuildAlgorithmVersion));
  hash = hashBytes(hash, &settings.requestedError, sizeof(settings.requestedError));
  hash = hashBytes(hash, &settings.maxLevel, sizeof(settings.maxLevel));
  hash = hashBytes(hash, &settings.narrowBandVoxels, sizeof(settings.narrowBandVoxels));
  hash = hashBytes(hash, &settings.guardLeafLevels, sizeof(settings.guardLeafLevels));
  hash = hashBytes(hash, &settings.sdfWorkerCount, sizeof(settings.sdfWorkerCount));
  hash = hashBytes(hash, &settings.maxRefinedCellsPerBrick, sizeof(settings.maxRefinedCellsPerBrick));
  hash = hashBytes(hash, &settings.surfaceCloseVoxels, sizeof(settings.surfaceCloseVoxels));
  return hash;
}

void setIdentity(float matrix[16])
{
  for (uint32_t i = 0u; i < 16u; ++i)
    matrix[i] = 0.0f;
  matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

} // namespace

std::vector<sdf::VSDFInstanceWriteData> SparseSDFBuilder::buildInstances(
    const VirtualGeometrySourceAsset &asset,
    const SparseSDFBuildSettings &settings)
{
  SparseSDFBuildSettings sanitized = settings;
  sanitized.requestedError = std::max(1.0e-6f, sanitized.requestedError);
  sanitized.maxLevel = std::min(sanitized.maxLevel, 4u);
  sanitized.narrowBandVoxels = std::max(1u, sanitized.narrowBandVoxels);
  sanitized.guardLeafLevels = std::min(sanitized.guardLeafLevels, sanitized.maxLevel);
  sanitized.sdfWorkerCount = std::max(1u, sanitized.sdfWorkerCount);
  sanitized.maxRefinedCellsPerBrick = std::max(1u, std::min(sanitized.maxRefinedCellsPerBrick, sdf::VSDF_BRICK_CELL_COUNT));
  sanitized.surfaceCloseVoxels = std::min(sanitized.surfaceCloseVoxels, sdf::VSDF_BRICK_RESOLUTION - 1u);
  sanitized.progressLogBrickInterval = std::max(1u, sanitized.progressLogBrickInterval);

  std::vector<sdf::VSDFInstanceWriteData> instances;
  instances.reserve(asset.shapes.size());

  if (sanitized.progressLogging)
  {
    std::cerr << "[VSDF] build begin shapes=" << asset.shapes.size()
              << " requestedError=" << sanitized.requestedError
              << " maxLevel=" << sanitized.maxLevel
              << " narrowBandVoxels=" << sanitized.narrowBandVoxels
              << " guardLeafLevels=" << sanitized.guardLeafLevels
              << " sdfWorkers=" << sanitized.sdfWorkerCount
              << " maxRefinedCellsPerBrick=" << sanitized.maxRefinedCellsPerBrick
              << " surfaceCloseVoxels=" << sanitized.surfaceCloseVoxels
              << '\n';
  }

  for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(asset.shapes.size()); ++shapeIndex)
  {
    BuildContext context;
    context.settings = sanitized;
    context.sourceShapeIndex = shapeIndex;
    context.outputInstanceIndex = static_cast<uint32_t>(instances.size());
    context.totalSourceShapes = static_cast<uint32_t>(asset.shapes.size());
    context.triangles = collectShapeTriangles(asset, asset.shapes[shapeIndex]);
    if (context.triangles.empty())
    {
      if (sanitized.progressLogging)
        std::cerr << "[VSDF] shape " << (shapeIndex + 1u) << "/" << asset.shapes.size() << " skipped: no triangles\n";
      continue;
    }

    if (sanitized.progressLogging)
    {
      std::cerr << "[VSDF] shape " << (shapeIndex + 1u) << "/" << asset.shapes.size()
                << " instance " << context.outputInstanceIndex
                << " begin triangles=" << context.triangles.size()
                << '\n';
    }

    cacheTriangleMetadata(context);
    context.bvh = buildTriangleBVH(context.triangles);
    if (sanitized.progressLogging)
    {
      std::cerr << "[VSDF] shape " << (shapeIndex + 1u) << "/" << asset.shapes.size()
                << " instance " << context.outputInstanceIndex
                << " BVH nodes=" << context.bvh.nodes.size()
                << '\n';
    }

    const Bounds bounds = computeBounds(context.triangles);
    context.triangleNormalSign = calibrateTriangleNormalSign(context, bounds);
    buildParityGrid(context, bounds);
    const float instanceDiagonal = std::max(diagonalLength(bounds), 1.0e-6f);
    context.requestedErrorWorld = sanitized.requestedError;
    context.rootVolume = boundsVolume(bounds);
    if (sanitized.progressLogging)
    {
      std::cerr << "[VSDF] shape " << (shapeIndex + 1u) << "/" << asset.shapes.size()
                << " instance " << context.outputInstanceIndex
                << " boundsDiagonal=" << instanceDiagonal
                << " targetVolumeRatio=" << sanitized.requestedError
                << '\n';
    }
    buildNode(context, bounds, 0u);
    emitAllBrickSDFs(context);
    if (sanitized.progressLogging)
    {
      std::cerr << "[VSDF] shape " << (shapeIndex + 1u) << "/" << asset.shapes.size()
                << " instance " << context.outputInstanceIndex
                << " completed bricks=" << context.bricks.size()
                << " nodes=" << context.nodes.size()
                << " maxDepth=" << context.maxDepthBuilt;
      for (size_t depth = 0u; depth < context.depthBrickCounts.size(); ++depth)
        std::cerr << " depth" << depth << "=" << context.depthBrickCounts[depth];
      std::cerr << '\n';
    }

    sdf::VSDFInstanceWriteData instance;
    instance.directory.instanceId = static_cast<uint32_t>(instances.size());
    instance.directory.sourceMeshIndex = shapeIndex;
    instance.directory.materialRangeBegin = asset.shapes[shapeIndex].materialIndex;
    instance.directory.materialRangeCount = 1u;
    instance.directory.maxLevel = sanitized.maxLevel;
    instance.directory.flags = 0u;
    instance.directory.conservativeScale = 1.0f;
    instance.directory.requestedError = sanitized.requestedError;
    instance.directory.worldBoundsMin[0] = bounds.min.x;
    instance.directory.worldBoundsMin[1] = bounds.min.y;
    instance.directory.worldBoundsMin[2] = bounds.min.z;
    instance.directory.worldBoundsMax[0] = bounds.max.x;
    instance.directory.worldBoundsMax[1] = bounds.max.y;
    instance.directory.worldBoundsMax[2] = bounds.max.z;

    instance.header.rootNode = 0u;
    instance.header.narrowBandVoxels = sanitized.narrowBandVoxels;
    instance.header.maxLevel = sanitized.maxLevel;
    instance.header.localBoundsMin[0] = bounds.min.x;
    instance.header.localBoundsMin[1] = bounds.min.y;
    instance.header.localBoundsMin[2] = bounds.min.z;
    instance.header.localBoundsMax[0] = bounds.max.x;
    instance.header.localBoundsMax[1] = bounds.max.y;
    instance.header.localBoundsMax[2] = bounds.max.z;
    instance.header.rootVoxelSize = maxExtent(bounds) / static_cast<float>(sdf::VSDF_BRICK_RESOLUTION);
    instance.header.requestedError = sanitized.requestedError;
    setIdentity(instance.header.localToWorld);
    setIdentity(instance.header.worldToLocal);

    instance.traceAabbTree.assign(sdf::VSDF_TRACE_AABB_TREE_WORD_COUNT, 0u);
    instance.tracePageTable.assign(sdf::VSDF_TRACE_ROOT_CELL_COUNT, sdf::VSDF_TRACE_INVALID_BRICK);
    instance.tracePages = {};
    instance.tracePageMasks = {};
    instance.traceBrickMap = {};
    instance.traceGrid = {};
    instance.nodes = std::move(context.nodes);
    instance.bricks = std::move(context.bricks);
    instance.distances = std::move(context.distances);
    instances.push_back(std::move(instance));
  }

  if (sanitized.progressLogging)
  {
    std::cerr << "[VSDF] build complete instances=" << instances.size() << '\n';
  }

  return instances;
}

bool SparseSDFBuilder::writeVSDF(
    const std::string &path,
    const VirtualGeometrySourceAsset &asset,
    const SparseSDFBuildSettings &settings,
    std::string *error)
{
  const std::vector<sdf::VSDFInstanceWriteData> instances = buildInstances(asset, settings);
  return sdf::SDFSceneFile::save(path, instances, hashAsset(asset), buildSettingsHash(settings), error);
}

uint64_t SparseSDFBuilder::buildSettingsHash(const SparseSDFBuildSettings &settings)
{
  return hashSettings(settings);
}

} // namespace virtualgeometry
