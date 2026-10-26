#include "procedural/SphericalVoronoiMesh.hpp"

#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/Predicates.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <utility>

namespace procedural
{
namespace
{

constexpr double kPi = 3.14159265358979323846264338327950288;

template <typename Function> void parallelFor(const uint32_t count, const uint32_t requestedWorkers, Function &&function)
{
  const uint32_t available = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t workerCount = std::min(count, requestedWorkers == 0u ? std::min(64u, available * 2u) : std::clamp(requestedWorkers, 1u, 64u));
  if (workerCount <= 1u || count < 128u)
  {
    for (uint32_t index = 0u; index < count; ++index)
      function(index);
    return;
  }
  std::atomic<uint32_t> next{0u};
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (uint32_t worker = 0u; worker < workerCount; ++worker)
    workers.emplace_back(
        [&]
        {
          for (;;)
          {
            const uint32_t index = next.fetch_add(1u, std::memory_order_relaxed);
            if (index >= count)
              break;
            function(index);
          }
        });
  for (std::thread &worker : workers)
    worker.join();
}

uint64_t splitmix64(uint64_t value)
{
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

double hashSigned(const uint64_t value)
{
  const double unit = static_cast<double>(splitmix64(value) >> 11u) * (1.0 / 9007199254740992.0);
  return unit * 2.0 - 1.0;
}

SpherePoint add(const SpherePoint &a, const SpherePoint &b)
{
  return SpherePoint{a.x + b.x, a.y + b.y, a.z + b.z};
}

SpherePoint subtract(const SpherePoint &a, const SpherePoint &b)
{
  return SpherePoint{a.x - b.x, a.y - b.y, a.z - b.z};
}

SpherePoint multiply(const SpherePoint &value, const double scale)
{
  return SpherePoint{value.x * scale, value.y * scale, value.z * scale};
}

double dot(const SpherePoint &a, const SpherePoint &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

SpherePoint cross(const SpherePoint &a, const SpherePoint &b)
{
  return SpherePoint{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

SpherePoint normalize(const SpherePoint &value)
{
  const double length = std::sqrt(dot(value, value));
  if (length <= 1.0e-12)
    return SpherePoint{0.0, 0.0, 1.0};
  return multiply(value, 1.0 / length);
}

Point2 stereographicProjection(const SpherePoint &point)
{
  const double denominator = std::max(1.0e-8, 1.0 - point.z);
  return Point2{point.x / denominator, point.y / denominator};
}

uint64_t edgeKey(const uint32_t a, const uint32_t b)
{
  const uint32_t lo = std::min(a, b);
  const uint32_t hi = std::max(a, b);
  return (static_cast<uint64_t>(lo) << 32u) | hi;
}

void orientOutward(std::array<uint32_t, 3> &triangle, const std::vector<SpherePoint> &regions)
{
  const SpherePoint &a = regions[triangle[0]];
  const SpherePoint &b = regions[triangle[1]];
  const SpherePoint &c = regions[triangle[2]];
  const SpherePoint normal = cross(subtract(b, a), subtract(c, a));
  const SpherePoint center = add(add(a, b), c);
  if (dot(normal, center) < 0.0)
    std::swap(triangle[1], triangle[2]);
}

SpherePoint triangleCenter(const std::array<uint32_t, 3> &triangle, const std::vector<SpherePoint> &regions)
{
  return normalize(add(add(regions[triangle[0]], regions[triangle[1]]), regions[triangle[2]]));
}

void uniqueSort(std::vector<uint32_t> &values)
{
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::vector<SpherePoint> fibonacciSphere(const uint32_t count, const double jitter, const uint64_t seed, const uint32_t workerCount)
{
  const uint32_t safeCount = std::max(8u, count);
  std::vector<SpherePoint> points(safeCount);
  const double goldenAngle = kPi * (3.0 - std::sqrt(5.0));
  const double jitterAmount = std::clamp(jitter, 0.0, 1.0);
  parallelFor(safeCount, workerCount, [&](const uint32_t index)
  {
      const double t = (static_cast<double>(index) + 0.5) / static_cast<double>(safeCount);
      double z = 1.0 - 2.0 * t;
      double longitude = static_cast<double>(index) * goldenAngle;
      if (jitterAmount > 0.0)
      {
        const double step = 2.0 / static_cast<double>(safeCount);
        z = std::clamp(z + hashSigned(seed ^ (index * 0x9e3779b97f4a7c15ull)) * step * 0.42 * jitterAmount, -0.999999, 0.999999);
        longitude += hashSigned(seed ^ 0xbf58476d1ce4e5b9ull ^ (index * 0x94d049bb133111ebull)) * goldenAngle * 0.34 * jitterAmount;
      }
      const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
      points[index] = SpherePoint{std::cos(longitude) * radius, std::sin(longitude) * radius, z};
  });
  return points;
}

void finishRegionTopology(SphericalVoronoiMesh &mesh, const uint32_t workerCount)
{
  parallelFor(static_cast<uint32_t>(mesh.regionTriangles.size()), workerCount, [&](const uint32_t region)
  {
    uniqueSort(mesh.regionNeighbours[region]);
    std::vector<uint32_t> &triangles = mesh.regionTriangles[region];
    uniqueSort(triangles);
    const SpherePoint normal = mesh.regions[region];
    SpherePoint axisU = cross(SpherePoint{0.0, 0.0, 1.0}, normal);
    if (dot(axisU, axisU) < 1.0e-8)
      axisU = cross(SpherePoint{0.0, 1.0, 0.0}, normal);
    axisU = normalize(axisU);
    const SpherePoint axisV = normalize(cross(normal, axisU));
    std::vector<std::pair<double, uint32_t>> ordered;
    ordered.reserve(triangles.size());
    for (const uint32_t triangle : triangles)
    {
      const SpherePoint direction = subtract(mesh.triangleCenters[triangle], mesh.regions[region]);
      ordered.emplace_back(std::atan2(dot(direction, axisV), dot(direction, axisU)), triangle);
    }
    std::sort(ordered.begin(), ordered.end());
    for (size_t index = 0u; index < ordered.size(); ++index)
      triangles[index] = ordered[index].second;
  });
}

} // namespace

bool SphericalVoronoiMeshGenerator::generate(const SphericalVoronoiMeshSettings &settings, SphericalVoronoiMesh &mesh) const
{
  mesh = {};
  const uint32_t requestedRegions = std::max(8u, settings.regionCount);
  mesh.regions = fibonacciSphere(requestedRegions, settings.jitter, settings.seed, settings.workerCount);

  std::vector<Point2> projected;
  projected.reserve(mesh.regions.size());
  for (const SpherePoint &point : mesh.regions)
    projected.push_back(stereographicProjection(point));

  ConstrainedDelaunayTriangulator triangulator;
  ConstrainedDelaunayTriangulator::Settings triangulationSettings{};
  triangulationSettings.ensureDelaunay = true;
  triangulationSettings.maximumIterations = 0u;
  if (!triangulator.triangulate(projected, {}, triangulationSettings))
    return false;

  std::vector<uint64_t> edgeKeys;
  const std::vector<int64_t> rawTriangles = triangulator.collectTriangles();
  mesh.triangles.reserve(rawTriangles.size() / 3u + requestedRegions / 8u);
  edgeKeys.reserve(rawTriangles.size());
  for (size_t raw = 0u; raw + 2u < rawTriangles.size(); raw += 3u)
  {
    const int64_t ai = rawTriangles[raw];
    const int64_t bi = rawTriangles[raw + 1u];
    const int64_t ci = rawTriangles[raw + 2u];
    if (ai < 0 || bi < 0 || ci < 0 || ai >= requestedRegions || bi >= requestedRegions || ci >= requestedRegions)
      continue;
    std::array<uint32_t, 3> triangle{static_cast<uint32_t>(ai), static_cast<uint32_t>(bi), static_cast<uint32_t>(ci)};
    orientOutward(triangle, mesh.regions);
    mesh.triangles.push_back(triangle);
    for (uint32_t edge = 0u; edge < 3u; ++edge)
      edgeKeys.push_back(edgeKey(triangle[edge], triangle[(edge + 1u) % 3u]));
  }
  if (mesh.triangles.empty())
    return false;

  const uint32_t poleRegion = static_cast<uint32_t>(mesh.regions.size());
  mesh.regions.push_back(SpherePoint{0.0, 0.0, 1.0});
  std::sort(edgeKeys.begin(), edgeKeys.end());
  for (size_t begin = 0u; begin < edgeKeys.size();)
  {
    size_t end = begin + 1u;
    while (end < edgeKeys.size() && edgeKeys[end] == edgeKeys[begin])
      ++end;
    const uint64_t key = edgeKeys[begin];
    const size_t useCount = end - begin;
    begin = end;
    if (useCount != 1u)
      continue;
    const uint32_t a = static_cast<uint32_t>(key >> 32u);
    const uint32_t b = static_cast<uint32_t>(key & 0xffffffffu);
    std::array<uint32_t, 3> cap{a, b, poleRegion};
    orientOutward(cap, mesh.regions);
    mesh.triangles.push_back(cap);
  }

  mesh.triangleCenters.resize(mesh.triangles.size());
  mesh.regionNeighbours.assign(mesh.regions.size(), {});
  mesh.regionTriangles.assign(mesh.regions.size(), {});
  parallelFor(static_cast<uint32_t>(mesh.triangles.size()), settings.workerCount, [&](const uint32_t triangleIndex)
  {
    mesh.triangleCenters[triangleIndex] = triangleCenter(mesh.triangles[triangleIndex], mesh.regions);
  });
  for (uint32_t triangleIndex = 0u; triangleIndex < mesh.triangles.size(); ++triangleIndex)
  {
    const std::array<uint32_t, 3> &triangle = mesh.triangles[triangleIndex];
    for (uint32_t corner = 0u; corner < 3u; ++corner)
    {
      const uint32_t region = triangle[corner];
      const uint32_t next = triangle[(corner + 1u) % 3u];
      const uint32_t previous = triangle[(corner + 2u) % 3u];
      mesh.regionTriangles[region].push_back(triangleIndex);
      mesh.regionNeighbours[region].push_back(next);
      mesh.regionNeighbours[region].push_back(previous);
    }
  }
  finishRegionTopology(mesh, settings.workerCount);
  return true;
}

} // namespace procedural
