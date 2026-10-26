#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "editor/virtualgeometry/SparseSDFBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "sdf/SDFSceneManager.hpp"
#include "sdf/rendering/SDFSceneGridBuildPass.hpp"
#include "sdf/rendering/SDFTracePass.hpp"

namespace
{

namespace fs = std::filesystem;
namespace vulkan = rendering::backend::vulkan;
using TraceRayGPU = gpusigneddistancefields::gpgpu::TraceRayGPU;
using TraceHitGPU = gpusigneddistancefields::gpgpu::TraceHitGPU;

struct Triangle
{
  math::Vec3f a{0.0f};
  math::Vec3f b{0.0f};
  math::Vec3f c{0.0f};
  uint32_t primitiveIndex = 0u;
};

struct RayExpectation
{
  TraceRayGPU ray{};
  Triangle triangle{};
};

struct TriangleBounds
{
  math::Vec3f min{std::numeric_limits<float>::max()};
  math::Vec3f max{std::numeric_limits<float>::lowest()};
};

struct TriangleRayHit
{
  bool hit = false;
  float t = std::numeric_limits<float>::max();
  math::Vec3f position{0.0f};
  Triangle triangle{};
};

struct CameraTraceReference
{
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<TraceRayGPU> rays;
  std::vector<TriangleRayHit> triangleHits;
};

struct TriangleBVHNode
{
  TriangleBounds bounds;
  uint32_t first = 0u;
  uint32_t count = 0u;
  uint32_t left = UINT32_MAX;
  uint32_t right = UINT32_MAX;
};

struct TriangleBVH
{
  std::vector<TriangleBVHNode> nodes;
  std::vector<uint32_t> indices;
};

bool isExpectedGpuBackendUnavailable(const std::exception &error)
{
  const std::string message = error.what();
#if defined(__APPLE__)
  if (message.find("failed to create instance") != std::string::npos)
    return true;
#endif
  return message.find("Vulkan is not supported on this device") != std::string::npos;
}

void expectTrue(bool condition, const std::string &message)
{
  if (!condition)
  {
    std::cerr << "  [FAIL] " << message << "\n";
    throw std::runtime_error(message);
  }
}

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path stanfordBunnyAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/stanford-bunny.obj";
}

math::Vec3f vertexPosition(const virtualgeometry::Vertex &vertex)
{
  return math::Vec3f(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
}

float triangleArea2(const Triangle &triangle)
{
  return (triangle.b - triangle.a).cross(triangle.c - triangle.a).length();
}

math::Vec3f triangleCentroid(const Triangle &triangle)
{
  return (triangle.a + triangle.b + triangle.c) * (1.0f / 3.0f);
}

TriangleBounds emptyTriangleBounds()
{
  return TriangleBounds{};
}

void includePoint(TriangleBounds &bounds, const math::Vec3f &point)
{
  bounds.min = {std::min(bounds.min[0], point[0]), std::min(bounds.min[1], point[1]), std::min(bounds.min[2], point[2])};
  bounds.max = {std::max(bounds.max[0], point[0]), std::max(bounds.max[1], point[1]), std::max(bounds.max[2], point[2])};
}

void includeBounds(TriangleBounds &bounds, const TriangleBounds &other)
{
  includePoint(bounds, other.min);
  includePoint(bounds, other.max);
}

TriangleBounds triangleBounds(const Triangle &triangle)
{
  TriangleBounds bounds = emptyTriangleBounds();
  includePoint(bounds, triangle.a);
  includePoint(bounds, triangle.b);
  includePoint(bounds, triangle.c);
  return bounds;
}

TriangleBounds buildSceneTriangleBounds(const std::vector<Triangle> &triangles)
{
  TriangleBounds bounds = emptyTriangleBounds();
  for (const Triangle &triangle : triangles)
    includeBounds(bounds, triangleBounds(triangle));
  return bounds;
}

float pointTriangleDistanceSquared(const math::Vec3f &p, const Triangle &triangle)
{
  const math::Vec3f ab = triangle.b - triangle.a;
  const math::Vec3f ac = triangle.c - triangle.a;
  const math::Vec3f ap = p - triangle.a;
  const float d1 = ab.dot(ap);
  const float d2 = ac.dot(ap);
  if (d1 <= 0.0f && d2 <= 0.0f)
    return (p - triangle.a).dot(p - triangle.a);

  const math::Vec3f bp = p - triangle.b;
  const float d3 = ab.dot(bp);
  const float d4 = ac.dot(bp);
  if (d3 >= 0.0f && d4 <= d3)
    return (p - triangle.b).dot(p - triangle.b);

  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
  {
    const float v = d1 / (d1 - d3);
    const math::Vec3f projection = triangle.a + ab * v;
    return (p - projection).dot(p - projection);
  }

  const math::Vec3f cp = p - triangle.c;
  const float d5 = ab.dot(cp);
  const float d6 = ac.dot(cp);
  if (d6 >= 0.0f && d5 <= d6)
    return (p - triangle.c).dot(p - triangle.c);

  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
  {
    const float w = d2 / (d2 - d6);
    const math::Vec3f projection = triangle.a + ac * w;
    return (p - projection).dot(p - projection);
  }

  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
  {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    const math::Vec3f projection = triangle.b + (triangle.c - triangle.b) * w;
    return (p - projection).dot(p - projection);
  }

  const float denom = 1.0f / (va + vb + vc);
  const float v = vb * denom;
  const float w = vc * denom;
  const math::Vec3f projection = triangle.a + ab * v + ac * w;
  return (p - projection).dot(p - projection);
}

std::vector<Triangle> collectTriangles(const virtualgeometry::VirtualGeometrySourceAsset &asset)
{
  std::vector<Triangle> triangles;
  uint32_t primitiveIndex = 0u;
  for (const virtualgeometry::SourceShapeBuildInput &shape : asset.shapes)
  {
    for (const virtualgeometry::MeshPart &part : shape.meshParts)
    {
      for (size_t index = 0u; index + 2u < part.shape.indices.size(); index += 3u)
      {
        const uint32_t ia = part.shape.indices[index + 0u];
        const uint32_t ib = part.shape.indices[index + 1u];
        const uint32_t ic = part.shape.indices[index + 2u];
        if (ia >= asset.vertices.size() || ib >= asset.vertices.size() || ic >= asset.vertices.size())
          continue;
        Triangle triangle{};
        triangle.a = vertexPosition(asset.vertices[ia]);
        triangle.b = vertexPosition(asset.vertices[ib]);
        triangle.c = vertexPosition(asset.vertices[ic]);
        triangle.primitiveIndex = primitiveIndex++;
        if (triangleArea2(triangle) > 1.0e-10f)
          triangles.push_back(triangle);
      }
    }
  }
  return triangles;
}

uint32_t buildTriangleBVHNode(
    TriangleBVH &bvh,
    const std::vector<Triangle> &triangles,
    uint32_t first,
    uint32_t count)
{
  const uint32_t nodeIndex = static_cast<uint32_t>(bvh.nodes.size());
  bvh.nodes.push_back(TriangleBVHNode{});

  TriangleBounds bounds = emptyTriangleBounds();
  TriangleBounds centroidBounds = emptyTriangleBounds();
  for (uint32_t i = first; i < first + count; ++i)
  {
    const Triangle &triangle = triangles[bvh.indices[i]];
    includeBounds(bounds, triangleBounds(triangle));
    includePoint(centroidBounds, triangleCentroid(triangle));
  }

  TriangleBVHNode &node = bvh.nodes[nodeIndex];
  node.bounds = bounds;
  node.first = first;
  node.count = count;

  constexpr uint32_t kLeafTriangleCount = 8u;
  if (count <= kLeafTriangleCount)
    return nodeIndex;

  const math::Vec3f centroidExtent = centroidBounds.max - centroidBounds.min;
  uint32_t splitAxis = 0u;
  if (centroidExtent[1] > centroidExtent[splitAxis])
    splitAxis = 1u;
  if (centroidExtent[2] > centroidExtent[splitAxis])
    splitAxis = 2u;

  const uint32_t mid = first + count / 2u;
  std::nth_element(
      bvh.indices.begin() + first,
      bvh.indices.begin() + mid,
      bvh.indices.begin() + first + count,
      [&triangles, splitAxis](uint32_t lhs, uint32_t rhs)
      {
        return triangleCentroid(triangles[lhs])[splitAxis] < triangleCentroid(triangles[rhs])[splitAxis];
      });

  node.count = 0u;
  node.left = buildTriangleBVHNode(bvh, triangles, first, mid - first);
  node.right = buildTriangleBVHNode(bvh, triangles, mid, first + count - mid);
  return nodeIndex;
}

TriangleBVH buildTriangleBVH(const std::vector<Triangle> &triangles)
{
  TriangleBVH bvh;
  bvh.indices.resize(triangles.size());
  for (uint32_t i = 0u; i < bvh.indices.size(); ++i)
    bvh.indices[i] = i;
  bvh.nodes.reserve(triangles.size() / 4u);
  if (!triangles.empty())
    buildTriangleBVHNode(bvh, triangles, 0u, static_cast<uint32_t>(triangles.size()));
  return bvh;
}

bool intersectRayBounds(
    const math::Vec3f &origin,
    const math::Vec3f &direction,
    const TriangleBounds &bounds,
    float tMin,
    float tMax)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (std::fabs(direction[axis]) < 1.0e-8f)
    {
      if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis])
        return false;
      continue;
    }

    const float invD = 1.0f / direction[axis];
    float t0 = (bounds.min[axis] - origin[axis]) * invD;
    float t1 = (bounds.max[axis] - origin[axis]) * invD;
    if (t0 > t1)
      std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax)
      return false;
  }
  return true;
}

bool intersectRayTriangle(
    const math::Vec3f &origin,
    const math::Vec3f &direction,
    const Triangle &triangle,
    float tMin,
    float tMax,
    float *outT)
{
  const math::Vec3f edge1 = triangle.b - triangle.a;
  const math::Vec3f edge2 = triangle.c - triangle.a;
  const math::Vec3f pvec = direction.cross(edge2);
  const float det = edge1.dot(pvec);
  if (std::fabs(det) < 1.0e-8f)
    return false;

  const float invDet = 1.0f / det;
  const math::Vec3f tvec = origin - triangle.a;
  const float u = tvec.dot(pvec) * invDet;
  if (u < 0.0f || u > 1.0f)
    return false;

  const math::Vec3f qvec = tvec.cross(edge1);
  const float v = direction.dot(qvec) * invDet;
  if (v < 0.0f || u + v > 1.0f)
    return false;

  const float t = edge2.dot(qvec) * invDet;
  if (t < tMin || t > tMax)
    return false;

  *outT = t;
  return true;
}

TriangleRayHit traceTriangleBVH(
    const TriangleBVH &bvh,
    const std::vector<Triangle> &triangles,
    const math::Vec3f &origin,
    const math::Vec3f &direction,
    float tMax)
{
  TriangleRayHit best{};
  if (bvh.nodes.empty())
    return best;

  std::array<uint32_t, 64> stack{};
  uint32_t stackSize = 0u;
  stack[stackSize++] = 0u;
  while (stackSize > 0u)
  {
    const TriangleBVHNode &node = bvh.nodes[stack[--stackSize]];
    if (!intersectRayBounds(origin, direction, node.bounds, 0.0f, std::min(best.t, tMax)))
      continue;

    if (node.count > 0u)
    {
      for (uint32_t i = node.first; i < node.first + node.count; ++i)
      {
        const Triangle &triangle = triangles[bvh.indices[i]];
        float t = 0.0f;
        if (intersectRayTriangle(origin, direction, triangle, 0.0f, std::min(best.t, tMax), &t))
        {
          best.hit = true;
          best.t = t;
          best.position = origin + direction * t;
          best.triangle = triangle;
        }
      }
      continue;
    }

    if (node.left != UINT32_MAX && stackSize < stack.size())
      stack[stackSize++] = node.left;
    if (node.right != UINT32_MAX && stackSize < stack.size())
      stack[stackSize++] = node.right;
  }
  return best;
}

float sceneDiagonal(const sdf::SDFSceneManager &manager)
{
  return (manager.worldBounds().max - manager.worldBounds().min).length();
}

TraceRayGPU makeTraceRay(const math::Vec3f &origin, const math::Vec3f &direction, float tMax)
{
  const math::Vec3f normalizedDirection = direction.normalize();
  TraceRayGPU ray{};
  ray.originAndMinT = {origin[0], origin[1], origin[2], 0.0f};
  ray.directionAndMaxT = {normalizedDirection[0], normalizedDirection[1], normalizedDirection[2], tMax};
  ray.metadata = {gpusigneddistancefields::gpgpu::TraceRayFlag_Active, 0u, 0u, 0u};
  return ray;
}

std::vector<RayExpectation> makeBunnyRayExpectations(
    const std::vector<Triangle> &triangles,
    const sdf::SDFSceneManager &manager)
{
  std::vector<uint32_t> order(triangles.size());
  for (uint32_t i = 0u; i < order.size(); ++i)
    order[i] = i;
  std::mt19937 rng(1337u);
  std::shuffle(order.begin(), order.end(), rng);

  const float diagonal = std::max(sceneDiagonal(manager), 1.0e-3f);
  const float originDistance = diagonal * 0.08f;
  const float maxDistance = diagonal * 0.25f;
  const float cpuAcceptanceDistance = diagonal * 0.08f;

  std::vector<RayExpectation> expectations;
  for (const uint32_t triangleIndex : order)
  {
    const Triangle &triangle = triangles[triangleIndex];
    const math::Vec3f normal = (triangle.b - triangle.a).cross(triangle.c - triangle.a).normalize();
    const math::Vec3f centroid = triangleCentroid(triangle);
    const math::Vec3f directions[2] = {normal * -1.0f, normal};
    const math::Vec3f origins[2] = {centroid + normal * originDistance, centroid - normal * originDistance};

    for (uint32_t side = 0u; side < 2u; ++side)
    {
      sdf::SDFRay cpuRay{};
      cpuRay.origin = origins[side];
      cpuRay.direction = directions[side].normalize();
      cpuRay.tMin = 0.0f;
      cpuRay.tMax = maxDistance;
      const sdf::SDFSceneHit cpuHit = manager.traceCPU(cpuRay);
      if (!cpuHit.hit)
        continue;
      if (std::sqrt(pointTriangleDistanceSquared(cpuHit.worldPosition, triangle)) > cpuAcceptanceDistance)
        continue;

      expectations.push_back(RayExpectation{
          .ray = makeTraceRay(cpuRay.origin, cpuRay.direction, maxDistance),
          .triangle = triangle,
      });
      break;
    }

    if (expectations.size() >= 8u)
      break;
  }

  return expectations;
}

std::vector<RayExpectation> makeRandomTrianglePiercingRays(
    const std::vector<Triangle> &triangles,
    const sdf::SDFSceneManager &manager,
    uint32_t targetRayCount)
{
  std::vector<uint32_t> order(triangles.size());
  for (uint32_t i = 0u; i < order.size(); ++i)
    order[i] = i;
  std::mt19937 rng(20260513u);
  std::shuffle(order.begin(), order.end(), rng);

  const float diagonal = std::max(sceneDiagonal(manager), 1.0e-3f);
  const float originDistance = diagonal * 0.10f;
  const float maxDistance = diagonal * 0.35f;
  const float cpuAcceptanceDistance = diagonal * 0.08f;
  std::vector<RayExpectation> rays;
  rays.reserve(targetRayCount);

  for (const uint32_t triangleIndex : order)
  {
    const Triangle &triangle = triangles[triangleIndex];
    const math::Vec3f normal = (triangle.b - triangle.a).cross(triangle.c - triangle.a).normalize();
    const math::Vec3f centroid = triangleCentroid(triangle);
    const bool flipFirst = (rng() & 1u) != 0u;

    for (uint32_t attempt = 0u; attempt < 2u; ++attempt)
    {
      const float side = ((attempt == 0u) == !flipFirst) ? 1.0f : -1.0f;
      const math::Vec3f origin = centroid + normal * (originDistance * side);
      const math::Vec3f direction = normal * -side;
      sdf::SDFRay cpuRay{};
      cpuRay.origin = origin;
      cpuRay.direction = direction.normalize();
      cpuRay.tMin = 0.0f;
      cpuRay.tMax = maxDistance;
      const sdf::SDFSceneHit cpuHit = manager.traceCPU(cpuRay);
      if (!cpuHit.hit)
        continue;
      if (std::sqrt(pointTriangleDistanceSquared(cpuHit.worldPosition, triangle)) > cpuAcceptanceDistance)
        continue;

      rays.push_back(RayExpectation{
          .ray = makeTraceRay(origin, direction, maxDistance),
          .triangle = triangle,
      });
      break;
    }

    if (rays.size() >= targetRayCount)
      break;
  }

  return rays;
}

std::vector<RayExpectation> makeGridPlaneTrianglePiercingRays(
    const std::vector<Triangle> &triangles,
    const sdf::SDFSceneManager &manager,
    uint32_t targetRayCount)
{
  std::vector<uint32_t> order(triangles.size());
  for (uint32_t i = 0u; i < order.size(); ++i)
    order[i] = i;
  std::mt19937 rng(20260514u);
  std::shuffle(order.begin(), order.end(), rng);

  const TriangleBounds bounds{
      .min = manager.worldBounds().min,
      .max = manager.worldBounds().max,
  };
  const math::Vec3f extent = bounds.max - bounds.min;
  const float diagonal = std::max(sceneDiagonal(manager), 1.0e-3f);
  const float originDistance = diagonal * 0.10f;
  const float maxDistance = diagonal * 0.35f;
  const float cpuAcceptanceDistance = diagonal * 0.08f;
  constexpr uint32_t kTraceGridResolution = 64u;

  std::vector<RayExpectation> rays;
  rays.reserve(targetRayCount);
  for (const uint32_t triangleIndex : order)
  {
    const Triangle &triangle = triangles[triangleIndex];
    const math::Vec3f normal = (triangle.b - triangle.a).cross(triangle.c - triangle.a).normalize();
    const TriangleBounds triBounds = triangleBounds(triangle);

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (std::fabs(normal[axis]) < 0.15f || extent[axis] <= 1.0e-8f)
        continue;

      const float cellSize = extent[axis] / static_cast<float>(kTraceGridResolution);
      const int firstPlane = std::max(1, static_cast<int>(std::ceil((triBounds.min[axis] - bounds.min[axis]) / cellSize)));
      const int lastPlane = std::min(static_cast<int>(kTraceGridResolution) - 1, static_cast<int>(std::floor((triBounds.max[axis] - bounds.min[axis]) / cellSize)));
      for (int planeIndex = firstPlane; planeIndex <= lastPlane; ++planeIndex)
      {
        const float plane = bounds.min[axis] + static_cast<float>(planeIndex) * cellSize;
        std::vector<math::Vec3f> intersections;
        intersections.reserve(3u);
        const auto addEdgeIntersection = [&](const math::Vec3f &p0, const math::Vec3f &p1)
        {
          const float d0 = p0[axis] - plane;
          const float d1 = p1[axis] - plane;
          const float denom = p1[axis] - p0[axis];
          if (std::fabs(denom) <= 1.0e-8f || d0 * d1 > 0.0f)
            return;
          const float t = (plane - p0[axis]) / denom;
          if (t < 0.0f || t > 1.0f)
            return;
          intersections.push_back(p0 + (p1 - p0) * t);
        };

        addEdgeIntersection(triangle.a, triangle.b);
        addEdgeIntersection(triangle.b, triangle.c);
        addEdgeIntersection(triangle.c, triangle.a);
        if (intersections.size() < 2u)
          continue;

        const math::Vec3f target = (intersections[0] + intersections[1]) * 0.5f;
        if (std::sqrt(pointTriangleDistanceSquared(target, triangle)) > cellSize * 0.01f)
          continue;

        for (float side : {1.0f, -1.0f})
        {
          const math::Vec3f origin = target + normal * (originDistance * side);
          const math::Vec3f direction = normal * -side;
          sdf::SDFRay cpuRay{};
          cpuRay.origin = origin;
          cpuRay.direction = direction.normalize();
          cpuRay.tMin = 0.0f;
          cpuRay.tMax = maxDistance;
          const sdf::SDFSceneHit cpuHit = manager.traceCPU(cpuRay);
          if (!cpuHit.hit)
            continue;
          if (std::sqrt(pointTriangleDistanceSquared(cpuHit.worldPosition, triangle)) > cpuAcceptanceDistance)
            continue;

          rays.push_back(RayExpectation{
              .ray = makeTraceRay(origin, direction, maxDistance),
              .triangle = triangle,
          });
          break;
        }

        if (rays.size() >= targetRayCount)
          return rays;
      }
    }
  }

  return rays;
}

CameraTraceReference makeBunnyCameraTraceReference(
    const std::vector<Triangle> &triangles,
    uint32_t width,
    uint32_t height)
{
  const TriangleBounds bounds = buildSceneTriangleBounds(triangles);
  const math::Vec3f center = (bounds.min + bounds.max) * 0.5f;
  const float diagonal = std::max((bounds.max - bounds.min).length(), 1.0e-3f);
  const float radius = diagonal * 0.5f;
  const float fovY = 40.0f * 3.14159265358979323846f / 180.0f;
  const float cameraDistance = (radius / std::tan(fovY * 0.5f)) * 1.35f;
  const math::Vec3f forward(0.0f, 0.0f, -1.0f);
  const math::Vec3f origin = center - forward * cameraDistance;
  const math::Vec3f worldUp(0.0f, 1.0f, 0.0f);
  const math::Vec3f right = forward.cross(worldUp).normalize();
  const math::Vec3f up = right.cross(forward).normalize();
  const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1u));
  const float tanHalfFovY = std::tan(fovY * 0.5f);
  const float tMax = cameraDistance + diagonal * 2.0f;
  const TriangleBVH bvh = buildTriangleBVH(triangles);

  CameraTraceReference reference;
  reference.width = width;
  reference.height = height;
  reference.rays.reserve(static_cast<size_t>(width) * height);
  reference.triangleHits.reserve(static_cast<size_t>(width) * height);
  for (uint32_t y = 0u; y < height; ++y)
  {
    for (uint32_t x = 0u; x < width; ++x)
    {
      const float px = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width) * 2.0f - 1.0f) * aspect * tanHalfFovY;
      const float py = (1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * 2.0f) * tanHalfFovY;
      const math::Vec3f direction = (forward + right * px + up * py).normalize();
      reference.rays.push_back(makeTraceRay(origin, direction, tMax));
      reference.triangleHits.push_back(traceTriangleBVH(bvh, triangles, origin, direction, tMax));
    }
  }
  return reference;
}

bool isConfidentCameraTriangleHit(const CameraTraceReference &reference, uint32_t pixelIndex, float depthTolerance)
{
  if (reference.width == 0u || reference.height == 0u || pixelIndex >= reference.triangleHits.size())
    return false;

  const uint32_t x = pixelIndex % reference.width;
  const uint32_t y = pixelIndex / reference.width;
  if (x == 0u || y == 0u || x + 1u >= reference.width || y + 1u >= reference.height)
    return false;

  const TriangleRayHit &center = reference.triangleHits[pixelIndex];
  if (!center.hit)
    return false;

  const uint32_t neighbors[4] = {
      pixelIndex - 1u,
      pixelIndex + 1u,
      pixelIndex - reference.width,
      pixelIndex + reference.width,
  };
  for (const uint32_t neighborIndex : neighbors)
  {
    const TriangleRayHit &neighbor = reference.triangleHits[neighborIndex];
    if (!neighbor.hit || std::fabs(neighbor.t - center.t) > depthTolerance)
      return false;
  }
  return true;
}

template <typename T>
std::vector<T> readBufferVector(rendering::RenderGraph &renderGraph, const rendering::Buffer &buffer, uint32_t count)
{
  std::vector<T> values(count);
  renderGraph.bufferRead(
      buffer,
      0u,
      static_cast<uint64_t>(count) * sizeof(T),
      [&](const void *data)
      {
        std::memcpy(values.data(), data, values.size() * sizeof(T));
      });
  return values;
}

void runGraph(rendering::RenderGraph &renderGraph)
{
  renderGraph.compile();
  rendering::RenderGraph::Frame frame;
  rendering::RenderGraph::Overrides overrides;
  renderGraph.run(frame, overrides);
  renderGraph.waitFrame(frame);
}

void assertBunnyTraceHits(
    rendering::RHI &rhi,
    const fs::path &vsdfPath,
    const sdf::SDFSceneManager &cpuManager,
    const std::vector<RayExpectation> &expectations,
    const std::string &label,
    uint32_t minHitCount)
{
  expectTrue(!expectations.empty(), label + " should provide SDF trace rays");

  std::string error;
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 16u, .maxFramesInFlight = 1u});
  sdf::SDFSceneManager gpuManager(&renderGraph);
  expectTrue(gpuManager.loadVSDF(vsdfPath.string(), &error), "Failed to load bunny sparse VSDF on GPU path: " + error);
  gpuManager.uploadGPUResources();

  std::vector<TraceRayGPU> rays;
  rays.reserve(expectations.size());
  for (const RayExpectation &expectation : expectations)
    rays.push_back(expectation.ray);

  const uint64_t rayBytes = static_cast<uint64_t>(rays.size()) * sizeof(TraceRayGPU);
  const uint64_t hitBytes = static_cast<uint64_t>(rays.size()) * sizeof(TraceHitGPU);
  const rendering::Buffer rayBuffer = renderGraph.createBuffer(
      rendering::BufferInfo{
          .name = label + ".rays",
          .size = std::max<uint64_t>(rayBytes, 4u),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  const rendering::Buffer hitBuffer = renderGraph.createBuffer(
      rendering::BufferInfo{
          .name = label + ".hits",
          .size = std::max<uint64_t>(hitBytes, 4u),
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  const rendering::Buffer hitReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
          .name = label + ".hits.readback",
          .size = std::max<uint64_t>(hitBytes, 4u),
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  renderGraph.bufferWrite(rayBuffer, 0u, rayBytes, rays.data());

  sdf::rendering::SDFTracePass::Settings traceSettings{};
  traceSettings.rayCount = static_cast<uint32_t>(rays.size());
  traceSettings.maxSteps = 256u;
  traceSettings.maxTraceDistance = std::max(sceneDiagonal(cpuManager), 1.0e-3f);
  traceSettings.hitThresholdScale = 1.0f;
  traceSettings.stepScale = 0.95f;
  traceSettings.minStepScale = 0.1f;
  renderGraph.registerPass<sdf::rendering::SDFSceneGridClearPass>(label + ".sceneGridClear", 0u, gpuManager);
  renderGraph.registerPass<sdf::rendering::SDFSceneGridBuildPass>(label + ".sceneGridBuild", 1u, gpuManager);
  renderGraph.registerPass<sdf::rendering::SDFTracePass>(label + ".trace", 2u, gpuManager, rayBuffer, hitBuffer, traceSettings);
  renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(label + ".copyHits", 3u, hitBuffer, 0u, hitBytes, hitReadback, 0u, hitBytes);

  runGraph(renderGraph);
  const std::vector<TraceHitGPU> hits = readBufferVector<TraceHitGPU>(renderGraph, hitReadback, static_cast<uint32_t>(rays.size()));
  const float allowedDistance = std::max(sceneDiagonal(cpuManager) * 0.13f, 1.0e-4f);
  uint32_t hitCount = 0u;
  uint32_t closeHitCount = 0u;
  uint32_t farHitCount = 0u;
  uint32_t adaptiveLevelHitCount = 0u;
  float maxObservedDistance = 0.0f;
  for (size_t i = 0u; i < hits.size(); ++i)
  {
    const TraceHitGPU &hit = hits[i];
    if ((hit.metadata[0] & gpusigneddistancefields::gpgpu::TraceHitFlag_Hit) == 0u)
      continue;

    ++hitCount;
    if (hit.traceInfo[0] != UINT32_MAX && hit.traceInfo[2] > 0u)
      ++adaptiveLevelHitCount;
    const math::Vec3f hitPosition(hit.worldPositionAndDistance[0], hit.worldPositionAndDistance[1], hit.worldPositionAndDistance[2]);
    const float distance = std::sqrt(pointTriangleDistanceSquared(hitPosition, expectations[i].triangle));
    maxObservedDistance = std::max(maxObservedDistance, distance);
    if (distance <= allowedDistance)
      ++closeHitCount;
    else
      ++farHitCount;
  }

  expectTrue(hitCount >= minHitCount, label + " expected at least " + std::to_string(minHitCount) + " GPU sparse SDF hits, got " + std::to_string(hitCount));
  expectTrue(adaptiveLevelHitCount > 0u, label + " expected at least one GPU sparse SDF hit from a refined adaptive BLAS level, got zero");
  expectTrue(closeHitCount >= minHitCount, label + " expected at least " + std::to_string(minHitCount) + " GPU sparse SDF hits close to bunny triangles, got close=" + std::to_string(closeHitCount) + " hits=" + std::to_string(hitCount) + " far=" + std::to_string(farHitCount) + " maxDistance=" + std::to_string(maxObservedDistance) + " allowed=" + std::to_string(allowedDistance));
  expectTrue(farHitCount <= std::max<uint32_t>(1u, hitCount / 8u), label + " expected far sparse SDF hits to stay rare, got far=" + std::to_string(farHitCount) + " hits=" + std::to_string(hitCount) + " maxDistance=" + std::to_string(maxObservedDistance) + " allowed=" + std::to_string(allowedDistance));
}

void assertBunnyCameraTraceMatchesTriangles(
    rendering::RHI &rhi,
    const fs::path &vsdfPath,
    const sdf::SDFSceneManager &cpuManager,
    const CameraTraceReference &reference,
    const std::string &label)
{
  expectTrue(reference.width == 256u && reference.height == 256u, label + " should use a 256x256 camera");
  expectTrue(reference.rays.size() == 256u * 256u, label + " should fire a 256x256 camera ray grid");
  expectTrue(reference.rays.size() == reference.triangleHits.size(), label + " ray/reference hit counts should match");

  uint32_t triangleHitCount = 0u;
  for (const TriangleRayHit &hit : reference.triangleHits)
    triangleHitCount += hit.hit ? 1u : 0u;
  expectTrue(triangleHitCount >= 1024u, label + " expected the camera to see at least 1024 bunny triangle pixels, got " + std::to_string(triangleHitCount));

  std::string error;
  rendering::RenderGraph renderGraph(&rhi, rendering::RenderGraph::Settings{.maxTimers = 16u, .maxFramesInFlight = 1u});
  sdf::SDFSceneManager gpuManager(&renderGraph);
  expectTrue(gpuManager.loadVSDF(vsdfPath.string(), &error), "Failed to load bunny sparse VSDF on GPU camera path: " + error);
  gpuManager.uploadGPUResources();

  const uint64_t rayBytes = static_cast<uint64_t>(reference.rays.size()) * sizeof(TraceRayGPU);
  const uint64_t hitBytes = static_cast<uint64_t>(reference.rays.size()) * sizeof(TraceHitGPU);
  const rendering::Buffer rayBuffer = renderGraph.createBuffer(
      rendering::BufferInfo{
          .name = label + ".rays",
          .size = rayBytes,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  const rendering::Buffer hitBuffer = renderGraph.createBuffer(
      rendering::BufferInfo{
          .name = label + ".hits",
          .size = hitBytes,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  const rendering::Buffer hitReadback = renderGraph.createBuffer(
      rendering::BufferInfo{
          .name = label + ".hits.readback",
          .size = hitBytes,
          .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
      });
  renderGraph.bufferWrite(rayBuffer, 0u, rayBytes, const_cast<TraceRayGPU *>(reference.rays.data()));

  sdf::rendering::SDFTracePass::Settings traceSettings{};
  traceSettings.rayCount = static_cast<uint32_t>(reference.rays.size());
  traceSettings.maxSteps = 256u;
  traceSettings.maxTraceDistance = std::max(sceneDiagonal(cpuManager) * 4.0f, 1.0e-3f);
  traceSettings.hitThresholdScale = 0.25f;
  traceSettings.stepScale = 0.95f;
  traceSettings.minStepScale = 0.1f;
  renderGraph.registerPass<sdf::rendering::SDFSceneGridClearPass>(label + ".sceneGridClear", 0u, gpuManager);
  renderGraph.registerPass<sdf::rendering::SDFSceneGridBuildPass>(label + ".sceneGridBuild", 1u, gpuManager);
  renderGraph.registerPass<sdf::rendering::SDFTracePass>(label + ".trace", 2u, gpuManager, rayBuffer, hitBuffer, traceSettings);
  renderGraph.registerPass<rendering::gpgpu::CopyBufferPass>(label + ".copyHits", 3u, hitBuffer, 0u, hitBytes, hitReadback, 0u, hitBytes);

  runGraph(renderGraph);
  const std::vector<TraceHitGPU> hits = readBufferVector<TraceHitGPU>(renderGraph, hitReadback, static_cast<uint32_t>(reference.rays.size()));
  const float diagonal = std::max(sceneDiagonal(cpuManager), 1.0e-3f);
  const float allowedDistance = std::max(diagonal * 0.13f, 1.0e-4f);
  const float confidentDepthTolerance = diagonal * 0.025f;
  uint32_t matchedHitCount = 0u;
  uint32_t sdfHitCount = 0u;
  uint32_t falsePositiveSdfHitCount = 0u;
  uint32_t closeMatchedHitCount = 0u;
  uint32_t confidentTriangleHitCount = 0u;
  uint32_t confidentMatchedHitCount = 0u;
  uint32_t closeHitCount = 0u;
  float maxObservedDistance = 0.0f;
  for (size_t i = 0u; i < hits.size(); ++i)
  {
    const TraceHitGPU &hit = hits[i];
    if ((hit.metadata[0] & gpusigneddistancefields::gpgpu::TraceHitFlag_Hit) == 0u)
      continue;

    ++sdfHitCount;
    if (!reference.triangleHits[i].hit)
    {
      ++falsePositiveSdfHitCount;
      continue;
    }

    ++matchedHitCount;
    const bool confidentTriangleHit = isConfidentCameraTriangleHit(reference, static_cast<uint32_t>(i), confidentDepthTolerance);
    if (confidentTriangleHit)
      ++confidentMatchedHitCount;
    const math::Vec3f sdfPosition(hit.worldPositionAndDistance[0], hit.worldPositionAndDistance[1], hit.worldPositionAndDistance[2]);
    const float distance = (sdfPosition - reference.triangleHits[i].position).length();
    maxObservedDistance = std::max(maxObservedDistance, distance);
    if (distance <= allowedDistance)
      ++closeMatchedHitCount;
    if (confidentTriangleHit && distance <= allowedDistance)
      ++closeHitCount;
  }

  for (uint32_t i = 0u; i < reference.triangleHits.size(); ++i)
    confidentTriangleHitCount += isConfidentCameraTriangleHit(reference, i, confidentDepthTolerance) ? 1u : 0u;

  expectTrue(matchedHitCount >= triangleHitCount * 3u / 5u, label + " expected SDF hits for at least 60% of triangle-hit camera rays, got " + std::to_string(matchedHitCount) + "/" + std::to_string(triangleHitCount));
  expectTrue(closeMatchedHitCount >= matchedHitCount * 75u / 100u, label + " expected at least 75% of matched SDF camera hits close to triangle hits, got " + std::to_string(closeMatchedHitCount) + "/" + std::to_string(matchedHitCount) + " maxDistance=" + std::to_string(maxObservedDistance) + " allowed=" + std::to_string(allowedDistance));
  expectTrue(falsePositiveSdfHitCount <= triangleHitCount * 3u / 2u, label + " expected sparse SDF camera false-positive hits to stay bounded, got falsePositive=" + std::to_string(falsePositiveSdfHitCount) + " sdfHits=" + std::to_string(sdfHitCount) + " triangleHits=" + std::to_string(triangleHitCount));
  expectTrue(confidentTriangleHitCount >= 512u, label + " expected at least 512 stable interior triangle-hit camera rays, got " + std::to_string(confidentTriangleHitCount));
  expectTrue(confidentMatchedHitCount >= confidentTriangleHitCount * 7u / 10u, label + " expected SDF hits for at least 70% of stable interior camera rays, got " + std::to_string(confidentMatchedHitCount) + "/" + std::to_string(confidentTriangleHitCount));
  expectTrue(closeHitCount >= confidentMatchedHitCount * 75u / 100u, label + " expected at least 75% of stable matched SDF hits close to triangle hits, got " + std::to_string(closeHitCount) + "/" + std::to_string(confidentMatchedHitCount) + " maxDistance=" + std::to_string(maxObservedDistance) + " allowed=" + std::to_string(allowedDistance));
}

void testBunnySDFTraceKernel(rendering::RHI &rhi)
{
  const fs::path assetPath = stanfordBunnyAssetPath();
  expectTrue(fs::exists(assetPath), "Stanford bunny OBJ should exist: " + assetPath.string());

  const fs::path tempRoot = fs::temp_directory_path() / "SDFTraceKernelTests";
  const fs::path materialsDir = tempRoot / "materials";
  std::error_code ec;
  fs::remove_all(tempRoot, ec);
  fs::create_directories(materialsDir, ec);
  expectTrue(!ec, "Failed to create SDF trace test temp directory");

  const virtualgeometry::VirtualGeometrySourceAsset asset = virtualgeometry::VirtualGeometryEncoder::loadAssetFromOBJFile(assetPath.string(), materialsDir.string());
  const std::vector<Triangle> triangles = collectTriangles(asset);
  expectTrue(!triangles.empty(), "Bunny OBJ should provide indexed triangles");

  const fs::path vsdfPath = tempRoot / "stanford-bunny.sparse.vsdf";
  virtualgeometry::SparseSDFBuildSettings buildSettings{};
  buildSettings.maxLevel = 2u;
  buildSettings.requestedError = 0.0125f;
  buildSettings.narrowBandVoxels = 6u;
  buildSettings.maxRefinedCellsPerBrick = sdf::VSDF_BRICK_CELL_COUNT;
  buildSettings.progressLogging = false;
  buildSettings.sdfWorkerCount = 8u;
  std::string error;
  expectTrue(virtualgeometry::SparseSDFBuilder::writeVSDF(vsdfPath.string(), asset, buildSettings, &error), "Failed to build bunny sparse VSDF: " + error);

  sdf::SDFSceneManager cpuManager;
  expectTrue(cpuManager.loadVSDF(vsdfPath.string(), &error), "Failed to load bunny sparse VSDF on CPU: " + error);
  const std::vector<RayExpectation> expectations = makeBunnyRayExpectations(triangles, cpuManager);
  expectTrue(expectations.size() >= 4u, "Expected at least four bunny triangle rays that CPU SDF can trace");
  assertBunnyTraceHits(rhi, vsdfPath, cpuManager, expectations, "SDFTraceKernelTests.filtered", std::max<uint32_t>(4u, static_cast<uint32_t>(expectations.size()) * 3u / 4u));

  const std::vector<RayExpectation> randomPiercingRays = makeRandomTrianglePiercingRays(triangles, cpuManager, 64u);
  expectTrue(randomPiercingRays.size() >= 48u, "Expected at least 48 random triangle-piercing bunny rays that CPU SDF can trace");
  assertBunnyTraceHits(rhi, vsdfPath, cpuManager, randomPiercingRays, "SDFTraceKernelTests.randomTriangleRays", 48u);

  const std::vector<RayExpectation> gridPlanePiercingRays = makeGridPlaneTrianglePiercingRays(triangles, cpuManager, 96u);
  expectTrue(gridPlanePiercingRays.size() >= 64u, "Expected at least 64 trace-grid-plane bunny rays that CPU SDF can trace");
  assertBunnyTraceHits(rhi, vsdfPath, cpuManager, gridPlanePiercingRays, "SDFTraceKernelTests.gridPlaneTriangleRays", 64u);

  const CameraTraceReference cameraReference = makeBunnyCameraTraceReference(triangles, 256u, 256u);
  assertBunnyCameraTraceMatchesTriangles(rhi, vsdfPath, cpuManager, cameraReference, "SDFTraceKernelTests.camera256");

  fs::remove_all(tempRoot, ec);
}

} // namespace

int main()
{
  os::Logger::start(100);

  rendering::DeviceRequiredLimits limits{0u, 0u, 0u};
  rendering::DeviceFeatures features = rendering::DeviceFeatures::DeviceFeatures_Compute;
  std::unique_ptr<vulkan::VulkanRHI> rhi;
  try
  {
    rhi = std::make_unique<vulkan::VulkanRHI>(
        vulkan::Vulkan_1_2,
        limits,
        features,
        std::vector<std::string>{});
    std::vector<VkSurfaceKHR> surfaces;
    rhi->init(surfaces);
  }
  catch (const std::exception &gpuError)
  {
    if (!isExpectedGpuBackendUnavailable(gpuError))
      throw;

    os::Logger::warningf("[Test] SDF trace kernel tests skipped: %s", gpuError.what());
    os::Logger::shutdown();
    std::cout << "SDFTraceKernelTests passed (GPU backend skipped)\n";
    return 0;
  }

  testBunnySDFTraceKernel(*rhi);
  os::Logger::shutdown();
  std::cout << "SDFTraceKernelTests passed\n";
  return 0;
}
