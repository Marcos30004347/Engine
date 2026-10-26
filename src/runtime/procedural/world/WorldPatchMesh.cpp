#include "procedural/world/WorldPatchMesh.hpp"
#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/Geometry.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::rewrite
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
  return false;
}
using Ring = std::vector<uint32_t>;
using Edge = std::pair<uint32_t, uint32_t>;
Edge canonical(uint32_t a, uint32_t b)
{
  return {std::min(a, b), std::max(a, b)};
}
bool on(Point2 a, Point2 b, Point2 p)
{
  return orient2d(a, b, p) == 0 && onSegment(a, p, b);
}
bool crosses(Point2 a, Point2 b, Point2 c, Point2 d)
{
  const auto abC = orient2d(a, b, c), abD = orient2d(a, b, d), cdA = orient2d(c, d, a), cdB = orient2d(c, d, b);
  return (signOf(abC) * signOf(abD) < 0 && signOf(cdA) * signOf(cdB) < 0) || on(a, b, c) || on(a, b, d) || on(c, d, a) || on(c, d, b);
}
bool inside(Point2 p, const Ring &ring, const std::vector<Point2> &points)
{
  bool result = false;
  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const auto a = points[ring[j]], b = points[ring[i]];
    if (on(a, b, p))
      return true;
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      result = !result;
  }
  return result;
}
double area(const Ring &ring, const std::vector<Point2> &points)
{
  // Relative origin avoids cancellation for translated patches.
  const auto origin = points[ring[0]];
  double total = 0;
  for (size_t i = 1; i + 1 < ring.size(); ++i)
    total += orient2d(origin, points[ring[i]], points[ring[i + 1]]);
  return std::abs(total) * .5;
}
} // namespace
bool compilePatchMesh(const PatchDomain &domain, const BoundaryRegistry &registry, const std::vector<Position> &samples, const std::function<double(double, double)> &surface, PatchMesh &mesh, std::string *error)
{
  if (error)
    error->clear();
  if (!domain.id || !surface)
    return fail(error, "patch requires ID and surface");
  PatchMesh output;
  output.patchId = domain.id;
  std::vector<Point2> points;
  std::map<std::pair<uint64_t, uint64_t>, uint32_t> ids;
  std::map<std::pair<double, double>, uint32_t> positions;
  std::vector<Ring> rings;
  auto loops = domain.holes;
  loops.insert(loops.begin(), domain.outer);
  for (const auto &loop : loops)
  {
    if (loop.empty())
      return fail(error, "empty patch ring");
    Ring ring;
    uint32_t first = 0, previous = 0;
    bool started = false;
    for (const auto &use : loop)
    {
      const auto *edge = registry.find(use.edgeId);
      if (!edge || edge->vertices.size() < 2)
        return fail(error, "missing patch boundary");
      std::vector<uint32_t> indices;
      for (size_t j = 0; j < edge->vertices.size(); ++j)
      {
        const auto &v = edge->vertices[use.reversed ? edge->vertices.size() - 1 - j : j];
        auto found = ids.find({v.owner, v.index});
        uint32_t index;
        if (found == ids.end())
        {
          if (positions.count({v.position.x, v.position.z}))
            return fail(error, "distinct vertex IDs share a position");
          index = static_cast<uint32_t>(points.size());
          ids.emplace(std::make_pair(v.owner, v.index), index);
          positions.emplace(std::make_pair(v.position.x, v.position.z), index);
          points.push_back({v.position.x, v.position.z});
          output.vertices.push_back(v);
        }
        else
          index = found->second;
        indices.push_back(index);
      }
      if (started && previous != indices.front())
        return fail(error, "patch boundary chain is disconnected");
      if (!started)
      {
        first = indices.front();
        started = true;
      }
      for (size_t j = 0; j + 1 < indices.size(); ++j)
      {
        ring.push_back(indices[j]);
        output.constraints.push_back({edge->id, indices[j], indices[j + 1]});
      }
      previous = indices.back();
    }
    if (previous != first || ring.size() < 3)
      return fail(error, "patch ring is not closed");
    if (std::set<uint32_t>(ring.begin(), ring.end()).size() != ring.size())
      return fail(error, "patch ring repeats a vertex");
    rings.push_back(std::move(ring));
  }
  // Bound the reference all-pairs validation until the indexed arrangement lands.
  if (output.constraints.size() > 8192)
    return fail(error, "patch exceeds reference PSLG validation budget");
  const auto &segments = output.constraints;
  for (size_t i = 0; i < segments.size(); ++i)
    for (size_t j = i + 1; j < segments.size(); ++j)
    {
      const auto a = segments[i], b = segments[j];
      if (a.a == b.a || a.a == b.b || a.b == b.a || a.b == b.b)
      {
        const uint32_t shared = (a.a == b.a || a.a == b.b) ? a.a : a.b;
        const auto pa = points[a.a == shared ? a.b : a.a], pb = points[b.a == shared ? b.b : b.a], p = points[shared];
        if (on(p, pa, pb) || on(p, pb, pa))
          return fail(error, "overlapping patch constraints");
        continue;
      }
      if (crosses(points[a.a], points[a.b], points[b.a], points[b.b]))
        return fail(error, "crossing or touching patch constraints");
    }
  output.domainArea = area(rings[0], points);
  for (size_t i = 1; i < rings.size(); ++i)
  {
    if (!inside(points[rings[i][0]], rings[0], points))
      return fail(error, "hole outside patch");
    for (size_t j = 1; j < rings.size(); ++j)
      if (i != j && inside(points[rings[i][0]], rings[j], points))
        return fail(error, "nested or overlapping holes");
    // Touching rings through a shared endpoint are also unsupported.
    for (size_t j = 0; j < i; ++j)
      for (auto v : rings[i])
        if (std::find(rings[j].begin(), rings[j].end(), v) != rings[j].end())
          return fail(error, "patch rings touch");
    output.domainArea -= area(rings[i], points);
  }
  if (!(output.domainArea > 0) || !std::isfinite(output.domainArea))
    return fail(error, "invalid patch area");
  auto eligible = [&](Point2 p)
  {
    if (!inside(p, rings[0], points))
      return false;
    for (size_t i = 1; i < rings.size(); ++i)
      if (inside(p, rings[i], points))
        return false;
    return true;
  };
  for (size_t i = 0; i < samples.size(); ++i)
  {
    const auto &sample = samples[i];
    Point2 p{sample.x, sample.z};
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
      return fail(error, "nonfinite patch sample");
    if (!eligible(p))
      return fail(error, "sample outside usable patch");
    bool boundary = false;
    for (const auto &e : segments)
      boundary |= on(points[e.a], points[e.b], p);
    if (boundary)
      return fail(error, "interior sample on boundary must be registered first");
    if (positions.count({p.x, p.y}))
      return fail(error, "duplicate interior sample");
    const double h = surface(p.x, p.y);
    if (!std::isfinite(h))
      return fail(error, "nonfinite patch surface");
    positions.emplace(std::make_pair(p.x, p.y), static_cast<uint32_t>(points.size()));
    points.push_back(p);
    output.vertices.push_back({UINT64_MAX, uint64_t(i), {p.x, h, p.y}});
  }
  std::vector<Constraint> constraints;
  for (const auto &edge : segments)
    constraints.emplace_back(edge.a, edge.b);
  ConstrainedDelaunayTriangulator cdt;
  if (!cdt.triangulate(points, constraints))
    return fail(error, "CDT failed; constraints were not dropped");
  // Classify CDT faces topologically. A centroid of a nearly collinear
  // boundary triangle can round outside its own polygon. Crossing a registered
  // outer/hole segment toggles parity; all super-triangle faces start outside.
  std::set<Edge> barriers;
  for (const auto &edge : segments)
    barriers.insert(canonical(edge.a, edge.b));
  std::map<Edge, std::vector<uint64_t>> incident;
  std::vector<int> parity(cdt.triangleCount(), -1);
  std::vector<uint64_t> queue;
  for (uint64_t i = 0; i < cdt.triangleCount(); ++i)
    if (cdt.triangleExists(i))
    {
      bool outside = false;
      for (int k = 0; k < 3; ++k)
      {
        auto a = cdt.vertexOfTriangle(i, k), b = cdt.vertexOfTriangle(i, (k + 1) % 3);
        incident[canonical(uint32_t(a), uint32_t(b))].push_back(i);
        outside |= uint64_t(a) >= points.size();
      }
      if (outside)
      {
        parity[i] = 0;
        queue.push_back(i);
      }
    }
  for (auto edge : barriers)
    if (!incident.count(edge))
      return fail(error, "CDT omitted constraint before classification");
  for (size_t cursor = 0; cursor < queue.size(); ++cursor)
  {
    auto i = queue[cursor];
    for (int k = 0; k < 3; ++k)
    {
      auto edge = canonical(uint32_t(cdt.vertexOfTriangle(i, k)), uint32_t(cdt.vertexOfTriangle(i, (k + 1) % 3)));
      for (auto j : incident[edge])
        if (j != i)
        {
          int expected = parity[i] ^ int(barriers.count(edge));
          if (parity[j] < 0)
          {
            parity[j] = expected;
            queue.push_back(j);
          }
          else if (parity[j] != expected)
            return fail(error, "CDT inconsistent boundary parity");
        }
    }
  }
  std::set<Edge> emitted;
  for (uint64_t i = 0; i < cdt.triangleCount(); ++i)
  {
    if (!cdt.triangleExists(i))
      continue;
    std::array<uint32_t, 3> t;
    bool valid = true;
    for (int j = 0; j < 3; ++j)
    {
      auto v = cdt.vertexOfTriangle(i, j);
      valid &= v >= 0 && uint64_t(v) < points.size();
      t[j] = uint32_t(v);
    }
    if (!valid)
      continue;
    auto a = points[t[0]], b = points[t[1]], c = points[t[2]];
    if (parity[i] != 1)
      continue;
    const double signedArea = orient2d(a, b, c);
    if (signedArea == 0 || !std::isfinite(signedArea))
      return fail(error, "degenerate patch triangle");
    // Counterclockwise in x/z points down in x/y/z: reverse for upward normals.
    if (signedArea > 0)
      std::swap(t[1], t[2]);
    output.triangles.push_back(t);
    output.triangleArea += std::abs(signedArea) * .5;
    for (int j = 0; j < 3; ++j)
      emitted.insert(canonical(t[j], t[(j + 1) % 3]));
  }
  for (const auto &edge : segments)
    if (!emitted.count(canonical(edge.a, edge.b)))
      return fail(error, "CDT missing required edge " + std::to_string(edge.a) + "-" + std::to_string(edge.b) + " emitted=" + std::to_string(output.triangles.size()));
  if (std::abs(output.domainArea - output.triangleArea) > std::max(.01, output.domainArea * 1e-8))
    return fail(error, "patch area coverage mismatch");
  mesh = std::move(output);
  return true;
}
} // namespace procedural::world::rewrite
