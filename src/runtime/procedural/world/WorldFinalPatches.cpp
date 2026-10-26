#include "WorldFinalPatches.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
namespace procedural::world::rewrite
{
namespace
{
bool fail(std::string *error, const char *message)
{
  if (error)
    *error = message;
  return false;
}
constexpr double tolerance = 3e-6;
bool interval(Position a, Position b, Position p, Position q, double &lo, double &hi)
{
  double dx = b.x - a.x, dz = b.z - a.z, length = std::hypot(dx, dz);
  if (length <= tolerance)
    return false;
  auto station = [&](Position v)
  {
    return ((v.x - a.x) * dx + (v.z - a.z) * dz) / length;
  };
  auto distance = [&](Position v)
  {
    return std::abs(dx * (v.z - a.z) - dz * (v.x - a.x)) / length;
  };
  lo = std::min(station(p), station(q));
  hi = std::max(station(p), station(q));
  return distance(p) <= tolerance && distance(q) <= tolerance && lo >= -tolerance && hi <= length + tolerance && hi - lo > 1e-9;
}
bool same(Position a, Position b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z;
}
} // namespace
bool auditWorldFinalPatches(const WorldFinalPatches &p, std::string *error)
{
  const auto &settings = p.map.settings;
  auto cutsValid = [](const std::vector<double> &cuts, double lo, double hi)
  {
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
      return false;
    for (double value : cuts)
    {
      if (!std::isfinite(value) || !(value > lo && value < hi))
        return false;
      lo = value;
    }
    return true;
  };
  if (!cutsValid(settings.cutsX, settings.minX, settings.maxX) || !cutsValid(settings.cutsZ, settings.minZ, settings.maxZ))
    return fail(error, "invalid final tile partition");
  for (size_t i = 0; i < p.map.faces.size(); ++i)
  {
    const auto &face = p.map.faces[i];
    if (face.id != i + 1 || face.vertices.size() < 3 || face.layer > 2 || uint32_t(face.role) > uint32_t(SurfaceRole::Ditch) || !std::isfinite(face.plane.x) || !std::isfinite(face.plane.z) ||
        !std::isfinite(face.plane.constant))
      return fail(error, "invalid final face metadata");
    for (auto vertex : face.vertices)
      if (vertex >= p.map.vertices.size())
        return fail(error, "invalid final face vertex");
  }
  std::map<uint64_t, const SurfaceMapEdge *> edges;
  for (const auto &e : p.map.edges)
    if (!e.id || e.leftFace > p.map.faces.size() || e.rightFace > p.map.faces.size() || e.a >= p.map.vertices.size() || e.b >= p.map.vertices.size() || !edges.emplace(e.id, &e).second)
      return fail(error, "invalid final edge identity");
  if (p.mesh.patches.size() != p.map.faces.size())
    return fail(error, "missing final patch");
  if (p.mesh.vertices.size() < p.map.vertices.size())
    return fail(error, "missing frozen mesh vertices");
  for (size_t i = 0; i < p.map.vertices.size(); ++i)
    if (!same(p.mesh.vertices[i], p.map.vertices[i]))
      return fail(error, "published mesh moved frozen vertex");
  std::map<uint64_t, std::set<std::pair<uint32_t, uint32_t>>> publishedEdges;
  double groundArea = 0;
  for (const auto &t : p.mesh.triangles)
  {
    if (!t.face || t.face > p.map.faces.size())
      return fail(error, "invalid published triangle face");
    const auto &f = p.map.faces[t.face - 1];
    if (t.material != f.material || t.layer != f.layer || t.role != f.role)
      return fail(error, "published material ownership mismatch");
    for (int i = 0; i < 3; ++i)
    {
      if (t.vertices[i] >= p.mesh.vertices.size())
        return fail(error, "invalid published triangle vertex");
      publishedEdges[t.face].insert(std::minmax(t.vertices[i], t.vertices[(i + 1) % 3]));
    }
    auto a = p.mesh.vertices[t.vertices[0]], b = p.mesh.vertices[t.vertices[1]], c = p.mesh.vertices[t.vertices[2]];
    double area = ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z)) * .5;
    if (!std::isfinite(area) || area <= 0)
      return fail(error, "invalid published triangle area");
    if (!t.layer)
      groundArea += area;
  }
  const auto &bounds = p.map.settings;
  double expectedArea = (bounds.maxX - bounds.minX) * (bounds.maxZ - bounds.minZ);
  if (!std::isfinite(expectedArea) || expectedArea <= 0 || std::abs(groundArea - expectedArea) > expectedArea * 1e-7)
    return fail(error, "published ground coverage mismatch");
  for (const auto &e : p.map.edges)
    for (auto face : {e.leftFace, e.rightFace})
      if (face && !publishedEdges[face].count(std::minmax(e.a, e.b)))
        return fail(error, "published mesh lost canonical edge");
  std::map<uint64_t, std::set<uint64_t>> preserved;
  std::set<uint64_t> patchIds;
  for (const auto &patch : p.mesh.patches)
  {
    if (!patch.patchId || patch.patchId > p.map.faces.size() || !patchIds.insert(patch.patchId).second)
      return fail(error, "invalid final patch identity");
    std::set<std::pair<uint32_t, uint32_t>> triangleEdges;
    for (const auto &t : patch.triangles)
      for (int i = 0; i < 3; ++i)
      {
        if (t[i] >= patch.vertices.size())
          return fail(error, "invalid final triangle");
        triangleEdges.insert(std::minmax(t[i], t[(i + 1) % 3]));
      }
    for (const auto &c : patch.constraints)
    {
      auto it = edges.find(c.sourceEdge);
      if (it == edges.end() || c.a >= patch.vertices.size() || c.b >= patch.vertices.size() || !triangleEdges.count(std::minmax(c.a, c.b)))
        return fail(error, "CDT constraint missing from triangles");
      const auto &e = *it->second;
      auto a = patch.vertices[c.a].position, b = patch.vertices[c.b].position;
      auto u = p.map.vertices[e.a], v = p.map.vertices[e.b];
      // Canonical edges are already split at every station, so each is one CDT edge.
      if (!((same(a, u) && same(b, v)) || (same(a, v) && same(b, u))))
        return fail(error, "CDT moved canonical boundary");
      preserved[e.id].insert(patch.patchId);
    }
  }
  for (const auto &[id, e] : edges)
    for (auto face : {e->leftFace, e->rightFace})
      if (face && !preserved[id].count(face))
        return fail(error, "canonical boundary lost by CDT");
  std::set<std::tuple<uint64_t, uint64_t, uint32_t>> keys;
  for (const auto &source : p.sources)
  {
    if (!source.producer || !source.contour || !keys.emplace(source.producer, source.contour, source.segment).second)
      return fail(error, "duplicate final source identity");
    std::vector<std::pair<double, double>> ranges;
    std::set<uint64_t> unique;
    for (auto id : source.edges)
    {
      auto it = edges.find(id);
      double lo, hi;
      if (it == edges.end() || !unique.insert(id).second)
        return fail(error, "invalid source chain edge");
      const auto &e = *it->second;
      if (e.layer != source.layer || std::find(e.sourceContours.begin(), e.sourceContours.end(), source.canonicalContour) == e.sourceContours.end() ||
          !interval(source.from, source.to, p.map.vertices[e.a], p.map.vertices[e.b], lo, hi))
        return fail(error, "source chain moved or lost provenance");
      ranges.emplace_back(lo, hi);
    }
    std::sort(ranges.begin(), ranges.end());
    double end = 0;
    for (auto [lo, hi] : ranges)
    {
      if (lo > end + tolerance || lo < end - tolerance)
        return fail(error, "source chain gap or overlap");
      end = hi;
    }
    if (ranges.empty() || std::abs(end - std::hypot(source.to.x - source.from.x, source.to.z - source.from.z)) > tolerance)
      return fail(error, "source boundary not fully preserved");
  }
  // Every triangle belongs to exactly one tile; no triangle straddles its rectangle.
  const auto &s = p.map.settings;
  std::vector<double> xs{s.minX}, zs{s.minZ};
  xs.insert(xs.end(), s.cutsX.begin(), s.cutsX.end());
  xs.push_back(s.maxX);
  zs.insert(zs.end(), s.cutsZ.begin(), s.cutsZ.end());
  zs.push_back(s.maxZ);
  if (p.tiles.size() != (xs.size() - 1) * (zs.size() - 1))
    return fail(error, "missing final tile");
  std::vector<uint32_t> counts(p.mesh.triangles.size());
  std::set<std::pair<uint32_t, uint32_t>> tileKeys;
  std::map<uint64_t, size_t> faceTiles;
  for (size_t i = 0; i < p.tiles.size(); ++i)
    for (auto face : p.tiles[i].faces)
      if (!face || face > p.map.faces.size() || !faceTiles.emplace(face, i).second)
        return fail(error, "invalid tile face ownership");
  if (faceTiles.size() != p.map.faces.size())
    return fail(error, "missing tile face");
  for (size_t i = 0; i < p.tiles.size(); ++i)
  {
    std::set<uint64_t> expected;
    for (const auto &e : p.map.edges)
    {
      auto left = e.leftFace ? faceTiles.at(e.leftFace) : SIZE_MAX;
      auto right = e.rightFace ? faceTiles.at(e.rightFace) : SIZE_MAX;
      if (left != right && (left == i || right == i))
        expected.insert(e.id);
    }
    const auto &listed = p.tiles[i].boundaryEdges;
    if (expected != std::set<uint64_t>(listed.begin(), listed.end()) || expected.size() != listed.size())
      return fail(error, "tile seam manifest mismatch");
  }
  for (const auto &tile : p.tiles)
  {
    if (uint64_t(tile.x) + 1 >= xs.size() || uint64_t(tile.z) + 1 >= zs.size() || !tileKeys.emplace(tile.x, tile.z).second)
      return fail(error, "invalid final tile");
    for (auto i : tile.triangles)
    {
      if (i >= counts.size() || ++counts[i] != 1)
        return fail(error, "duplicate tile triangle");
      for (auto v : p.mesh.triangles[i].vertices)
      {
        if (v >= p.mesh.vertices.size())
          return fail(error, "invalid tile vertex");
        const auto a = p.mesh.vertices[v];
        if (a.x < xs[tile.x] - tolerance || a.x > xs[tile.x + 1] + tolerance || a.z < zs[tile.z] - tolerance || a.z > zs[tile.z + 1] + tolerance)
          return fail(error, "triangle crosses tile seam");
      }
    }
  }
  if (std::find(counts.begin(), counts.end(), 0) != counts.end())
    return fail(error, "unassigned tile triangle");
  return true;
}
bool freezeWorldPatches(
    std::vector<WorldContourBatch> batches,
    SurfaceMapSettings settings,
    const SurfaceRefinement &refinement,
    const std::function<double(double, double)> &terrain,
    uint64_t revision,
    WorldFinalPatches &out,
    std::string *error)
{
  if (batches.size() > 2048)
    return fail(error, "final producer budget exceeded");
  std::set<uint64_t> producers;
  for (const auto &b : batches)
    if (!b.producer || !producers.insert(b.producer).second)
      return fail(error, "invalid final producer identity");
  std::sort(
      batches.begin(),
      batches.end(),
      [](const auto &a, const auto &b)
      {
        return std::tie(a.priority, a.producer) < std::tie(b.priority, b.producer);
      });
  for (auto *cuts : {&settings.cutsX, &settings.cutsZ})
  {
    for (double v : *cuts)
      if (!std::isfinite(v))
        return fail(error, "nonfinite tile cut");
    std::sort(cuts->begin(), cuts->end());
    cuts->erase(std::unique(cuts->begin(), cuts->end()), cuts->end());
  }
  WorldFinalPatches result;
  std::vector<SurfaceContour> contours;
  for (auto &batch : batches)
  {
    std::set<uint64_t> local;
    for (auto c : batch.contours)
    {
      if (!c.id || !local.insert(c.id).second)
        return fail(error, "duplicate local contour identity");
      if (c.loop.size() < 3 || c.loop.size() > 256)
        return fail(error, "final contour vertex budget exceeded");
      auto original = c.id;
      c.id = contours.size() + 1;
      for (uint32_t i = 0; i < c.loop.size(); ++i)
        result.sources.push_back({batch.producer, original, c.id, i, c.layer, c.loop[i], c.loop[(i + 1) % c.loop.size()], {}});
      contours.push_back(std::move(c));
      if (contours.size() > 2048)
        return fail(error, "final contour budget exceeded");
    }
  }
  if (!arrangeSurfaceContours(contours, settings, terrain, revision, result.map, error) || !meshSurfaceArrangement(result.map, terrain, refinement, result.mesh, error))
    return false;
  std::map<uint64_t, std::vector<const SurfaceMapEdge *>> bySource;
  for (const auto &e : result.map.edges)
    for (auto source : e.sourceContours)
      bySource[source].push_back(&e);
  for (auto &source : result.sources)
  {
    std::vector<std::pair<double, uint64_t>> ordered;
    for (auto e : bySource[source.canonicalContour])
    {
      double lo, hi;
      if (interval(source.from, source.to, result.map.vertices[e->a], result.map.vertices[e->b], lo, hi))
        ordered.emplace_back(lo, e->id);
    }
    std::sort(ordered.begin(), ordered.end());
    for (auto [station, id] : ordered)
      source.edges.push_back(id);
  }
  const auto nx = settings.cutsX.size() + 1, nz = settings.cutsZ.size() + 1;
  for (uint32_t z = 0; z < nz; ++z)
    for (uint32_t x = 0; x < nx; ++x)
      result.tiles.push_back({x, z, {}, {}, {}});
  std::vector<size_t> faceTile(result.map.faces.size() + 1);
  for (const auto &f : result.map.faces)
  {
    Position center{};
    for (auto v : f.vertices)
    {
      center.x += result.map.vertices[v].x / f.vertices.size();
      center.z += result.map.vertices[v].z / f.vertices.size();
    }
    auto x = std::upper_bound(settings.cutsX.begin(), settings.cutsX.end(), center.x) - settings.cutsX.begin();
    auto z = std::upper_bound(settings.cutsZ.begin(), settings.cutsZ.end(), center.z) - settings.cutsZ.begin();
    faceTile[f.id] = z * nx + x;
    result.tiles[z * nx + x].faces.push_back(f.id);
  }
  for (const auto &e : result.map.edges)
  {
    auto left = e.leftFace ? faceTile[e.leftFace] : SIZE_MAX, right = e.rightFace ? faceTile[e.rightFace] : SIZE_MAX;
    if (left != right)
    {
      if (left != SIZE_MAX)
        result.tiles[left].boundaryEdges.push_back(e.id);
      if (right != SIZE_MAX)
        result.tiles[right].boundaryEdges.push_back(e.id);
    }
  }
  for (uint32_t i = 0; i < result.mesh.triangles.size(); ++i)
    result.tiles[faceTile[result.mesh.triangles[i].face]].triangles.push_back(i);
  if (!auditWorldFinalPatches(result, error))
    return false;
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
