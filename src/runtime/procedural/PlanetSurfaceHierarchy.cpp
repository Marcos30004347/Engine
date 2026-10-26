#include "procedural/PlanetSurfaceHierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace procedural
{
namespace
{

constexpr double kPi = 3.14159265358979323846264338327950288;

SpherePoint add(const SpherePoint &a, const SpherePoint &b)
{
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

SpherePoint multiply(const SpherePoint &point, const double scale)
{
  return {point.x * scale, point.y * scale, point.z * scale};
}

SpherePoint normalize(const SpherePoint &point)
{
  const double length = std::sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
  if (!(length > 1.0e-15))
    return {0.0, 0.0, 1.0};
  return multiply(point, 1.0 / length);
}

uint64_t directedEdgeKey(const uint32_t a, const uint32_t b)
{
  return (static_cast<uint64_t>(a) << 32u) | b;
}

uint32_t wrappedX(int64_t x, const uint32_t width)
{
  x %= static_cast<int64_t>(width);
  if (x < 0)
    x += width;
  return static_cast<uint32_t>(x);
}

} // namespace

bool buildPlanetSurfaceHalfedgeMesh(
    const std::vector<SpherePoint> &vertices,
    const std::vector<std::array<uint32_t, 3>> &triangles,
    PlanetSurfaceHalfedgeMesh &mesh)
{
  mesh = {};
  if (vertices.empty() || triangles.empty())
    return false;
  mesh.vertices = vertices;
  const size_t halfedgeCount = triangles.size() * 3u;
  if (halfedgeCount > std::numeric_limits<uint32_t>::max())
    return false;
  mesh.vertex.resize(halfedgeCount);
  mesh.next.resize(halfedgeCount);
  mesh.previous.resize(halfedgeCount);
  mesh.twin.assign(halfedgeCount, PlanetSurfaceHalfedgeMesh::noHalfedge);
  mesh.face.resize(halfedgeCount);
  mesh.faceCentres.resize(triangles.size());

  std::unordered_map<uint64_t, uint32_t> directedEdges;
  directedEdges.reserve(halfedgeCount);
  for (uint32_t face = 0u; face < triangles.size(); ++face)
  {
    const auto &indices = triangles[face];
    if (indices[0] >= vertices.size() || indices[1] >= vertices.size() || indices[2] >= vertices.size() ||
        indices[0] == indices[1] || indices[1] == indices[2] || indices[2] == indices[0])
      return false;
    const uint32_t base = face * 3u;
    mesh.faceCentres[face] = multiply(add(add(vertices[indices[0]], vertices[indices[1]]), vertices[indices[2]]), 1.0 / 3.0);
    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      const uint32_t halfedge = base + edge;
      const uint32_t a = indices[edge];
      const uint32_t b = indices[(edge + 1u) % 3u];
      mesh.vertex[halfedge] = a;
      mesh.next[halfedge] = base + (edge + 1u) % 3u;
      mesh.previous[halfedge] = base + (edge + 2u) % 3u;
      mesh.face[halfedge] = face;
      if (!directedEdges.emplace(directedEdgeKey(a, b), halfedge).second)
        return false;
    }
  }
  for (uint32_t halfedge = 0u; halfedge < halfedgeCount; ++halfedge)
  {
    const uint32_t a = mesh.vertex[halfedge];
    const uint32_t b = mesh.vertex[mesh.next[halfedge]];
    if (const auto reverse = directedEdges.find(directedEdgeKey(b, a)); reverse != directedEdges.end())
      mesh.twin[halfedge] = reverse->second;
  }
  return validatePlanetSurfaceHalfedgeMesh(mesh);
}

bool validatePlanetSurfaceHalfedgeMesh(const PlanetSurfaceHalfedgeMesh &mesh)
{
  const size_t count = mesh.vertex.size();
  if (count == 0u || mesh.next.size() != count || mesh.previous.size() != count || mesh.twin.size() != count || mesh.face.size() != count)
    return false;
  for (uint32_t halfedge = 0u; halfedge < count; ++halfedge)
  {
    if (mesh.vertex[halfedge] >= mesh.vertices.size() || mesh.next[halfedge] >= count || mesh.previous[halfedge] >= count || mesh.face[halfedge] >= mesh.faceCentres.size())
      return false;
    if (mesh.previous[mesh.next[halfedge]] != halfedge || mesh.next[mesh.previous[halfedge]] != halfedge)
      return false;
    if (mesh.twin[halfedge] != PlanetSurfaceHalfedgeMesh::noHalfedge &&
        (mesh.twin[halfedge] >= count || mesh.twin[mesh.twin[halfedge]] != halfedge))
      return false;
  }
  return true;
}

bool validPlanetSurfaceBisectorAddress(const PlanetSurfaceBisectorAddress &address)
{
  if (address.depth > 63u)
    return false;
  return address.path < (uint64_t{1u} << address.depth);
}

PlanetSurfaceBisectorAddress planetSurfaceBisectorChild(const PlanetSurfaceBisectorAddress &parent, const uint32_t child)
{
  if (!validPlanetSurfaceBisectorAddress(parent) || parent.depth >= 63u || child > 1u)
    return {parent.rootHalfedge, 64u, 0u};
  return {parent.rootHalfedge, parent.depth + 1u, (parent.path << 1u) | child};
}

PlanetSurfaceBisectorAddress planetSurfaceBisectorParent(const PlanetSurfaceBisectorAddress &child)
{
  if (!validPlanetSurfaceBisectorAddress(child) || child.depth == 0u)
    return child;
  return {child.rootHalfedge, child.depth - 1u, child.path >> 1u};
}

bool planetSurfaceBisectorAncestor(
    const PlanetSurfaceBisectorAddress &address,
    const uint32_t ancestorDepth,
    PlanetSurfaceBisectorAddress &ancestor)
{
  if (!validPlanetSurfaceBisectorAddress(address) || ancestorDepth > address.depth)
    return false;
  ancestor = {
    address.rootHalfedge,
    ancestorDepth,
    address.path >> (address.depth - ancestorDepth),
  };
  return true;
}

bool planetSurfaceBisectorTriangle(
    const PlanetSurfaceHalfedgeMesh &mesh,
    const PlanetSurfaceBisectorAddress &address,
    PlanetSurfaceTriangle &triangle)
{
  // Full topology validation belongs at mesh construction/load time. Leaf
  // evaluation is a hot path and must stay proportional to subdivision depth.
  const size_t halfedgeCount = mesh.vertex.size();
  if (!validPlanetSurfaceBisectorAddress(address) || address.rootHalfedge >= halfedgeCount ||
      mesh.next.size() != halfedgeCount || mesh.face.size() != halfedgeCount)
    return false;
  const uint32_t root = address.rootHalfedge;
  if (mesh.next[root] >= halfedgeCount || mesh.vertex[root] >= mesh.vertices.size() ||
      mesh.vertex[mesh.next[root]] >= mesh.vertices.size() || mesh.face[root] >= mesh.faceCentres.size())
    return false;
  triangle.vertices = {
    mesh.vertices[mesh.vertex[root]],
    mesh.vertices[mesh.vertex[mesh.next[root]]],
    mesh.faceCentres[mesh.face[root]],
  };
  for (uint32_t level = 0u; level < address.depth; ++level)
  {
    const uint32_t bit = static_cast<uint32_t>((address.path >> (address.depth - level - 1u)) & 1u);
    const auto current = triangle.vertices;
    const SpherePoint midpoint = multiply(add(current[0], current[1]), 0.5);
    triangle.vertices = bit == 0u ?
        std::array<SpherePoint, 3>{current[0], current[2], midpoint} :
        std::array<SpherePoint, 3>{current[2], current[1], midpoint};
  }
  // The subdivision matrices operate on the coarse polyhedron. Projection is
  // deliberately delayed until the leaf is evaluated, as in the paper.
  for (SpherePoint &vertex : triangle.vertices)
    vertex = normalize(vertex);
  return true;
}

bool samplePlanetSurfaceAtlas(
    const PlanetSurfaceAtlasView &atlas,
    const SpherePoint &direction,
    PlanetSurfaceSample &sample)
{
  const size_t texelCount = static_cast<size_t>(atlas.width) * atlas.height;
  if (atlas.width == 0u || atlas.height == 0u || atlas.tileSizePixels == 0u ||
      atlas.heightmap == nullptr || atlas.heightCount != texelCount ||
      atlas.materials == nullptr || atlas.materialCount != texelCount ||
      atlas.sourceRegions == nullptr || atlas.sourceRegionCount != texelCount)
    return false;
  const SpherePoint normal = normalize(direction);
  sample = {};
  sample.u = std::atan2(normal.z, normal.x) / (2.0 * kPi) + 0.5;
  sample.u -= std::floor(sample.u);
  sample.v = std::clamp(0.5 - std::asin(std::clamp(normal.y, -1.0, 1.0)) / kPi, 0.0, 1.0);

  const double fx = sample.u * atlas.width - 0.5;
  const double fy = std::clamp(sample.v * atlas.height - 0.5, 0.0, static_cast<double>(atlas.height - 1u));
  const int64_t x0Raw = static_cast<int64_t>(std::floor(fx));
  const uint32_t x0 = wrappedX(x0Raw, atlas.width);
  const uint32_t x1 = (x0 + 1u) % atlas.width;
  const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
  const uint32_t y1 = std::min(y0 + 1u, atlas.height - 1u);
  const double tx = fx - std::floor(fx);
  const double ty = fy - y0;
  const auto heightAt = [&atlas](const uint32_t x, const uint32_t y)
  {
    return static_cast<double>(atlas.heightmap[static_cast<size_t>(y) * atlas.width + x]);
  };
  const double top = heightAt(x0, y0) + (heightAt(x1, y0) - heightAt(x0, y0)) * tx;
  const double bottom = heightAt(x0, y1) + (heightAt(x1, y1) - heightAt(x0, y1)) * tx;
  sample.height = static_cast<uint16_t>(std::lround(std::clamp(top + (bottom - top) * ty, 0.0, 65535.0)));

  sample.texelX = static_cast<uint32_t>(std::floor(sample.u * atlas.width)) % atlas.width;
  sample.texelY = std::min(static_cast<uint32_t>(std::floor(sample.v * atlas.height)), atlas.height - 1u);
  const size_t nearest = static_cast<size_t>(sample.texelY) * atlas.width + sample.texelX;
  sample.material = atlas.materials[nearest];
  sample.sourceRegion = atlas.sourceRegions[nearest];
  sample.tileColumn = sample.texelX / atlas.tileSizePixels;
  sample.tileRow = sample.texelY / atlas.tileSizePixels;
  sample.tileLocalX = sample.texelX % atlas.tileSizePixels;
  sample.tileLocalY = sample.texelY % atlas.tileSizePixels;
  return true;
}

} // namespace procedural
