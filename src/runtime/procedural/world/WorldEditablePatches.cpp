#include "WorldEditablePatches.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
namespace procedural::world::rewrite
{
namespace
{
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
PatchBounds bounds(const EditablePatchSet &s, const EditablePatch &p)
{
  PatchBounds b{INFINITY, INFINITY, -INFINITY, -INFINITY};
  for (auto id : p.vertices)
  {
    auto v = s.vertices.at(id);
    b.minX = std::min(b.minX, v.x);
    b.maxX = std::max(b.maxX, v.x);
    b.minZ = std::min(b.minZ, v.z);
    b.maxZ = std::max(b.maxZ, v.z);
  }
  return b;
}
void expand(PatchBounds &a, PatchBounds b)
{
  a.minX = std::min(a.minX, b.minX);
  a.minZ = std::min(a.minZ, b.minZ);
  a.maxX = std::max(a.maxX, b.maxX);
  a.maxZ = std::max(a.maxZ, b.maxZ);
}
bool overlap(const EditablePatchSet &s, const EditablePatch &a, const EditablePatch &b)
{
  for (auto *p : {&a, &b})
    for (size_t i = 0; i < p->vertices.size(); ++i)
    {
      auto u = s.vertices.at(p->vertices[i]), v = s.vertices.at(p->vertices[(i + 1) % p->vertices.size()]);
      double x = -(v.z - u.z), z = v.x - u.x;
      double amin = INFINITY, amax = -INFINITY, bmin = INFINITY, bmax = -INFINITY;
      for (auto id : a.vertices)
      {
        auto q = s.vertices.at(id);
        auto d = q.x * x + q.z * z;
        amin = std::min(amin, d);
        amax = std::max(amax, d);
      }
      for (auto id : b.vertices)
      {
        auto q = s.vertices.at(id);
        auto d = q.x * x + q.z * z;
        bmin = std::min(bmin, d);
        bmax = std::max(bmax, d);
      }
      if (std::min(amax, bmax) - std::max(amin, bmin) <= 1e-6 * std::hypot(x, z))
        return false;
    }
  return true;
}
bool fail(std::string *e, const char *s)
{
  if (e)
    *e = s;
  return false;
}
} // namespace
bool validateEditablePatches(const EditablePatchSet &s, std::string *e)
{
  if (!s.revision || s.patches.size() > 4096 || s.vertices.size() > 16384)
    return fail(e, "invalid editable domain budget/revision");
  std::set<std::pair<double, double>> positions;
  for (auto &[id, v] : s.vertices)
    if (!id || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || std::abs(v.x) > 1e7 || std::abs(v.z) > 1e7 || !positions.emplace(v.x, v.z).second)
      return fail(e, "invalid or duplicate vertex");
  std::map<std::tuple<uint64_t, uint64_t, uint64_t>, int> edges;
  for (auto &[id, p] : s.patches)
  {
    if (!id || p.id != id || !p.revision || p.vertices.size() < 3 || p.vertices.size() > 256 || p.use < PatchLandUse::Unassigned || p.use > PatchLandUse::Protected)
      return fail(e, "invalid patch identity/size");
    std::set<uint64_t> unique;
    for (auto v : p.vertices)
      if (!s.vertices.count(v) || !unique.insert(v).second)
        return fail(e, "missing or repeated patch vertex");
    double area = 0;
    for (size_t i = 0; i < p.vertices.size(); ++i)
    {
      auto a = s.vertices.at(p.vertices[i]), b = s.vertices.at(p.vertices[(i + 1) % p.vertices.size()]);
      area += a.x * b.z - a.z * b.x;
    }
    if (area <= 1e-7)
      return fail(e, "degenerate patch");
    std::set<uint64_t> ancestors{id};
    auto parent = p.parent;
    while (parent)
    {
      if (!s.patches.count(parent) || !ancestors.insert(parent).second)
        return fail(e, "invalid parent hierarchy");
      parent = s.patches.at(parent).parent;
    }
    if (p.parent)
    {
      auto &outer = s.patches.at(p.parent);
      for (size_t i = 0; i < outer.vertices.size(); ++i)
        for (auto v : p.vertices)
          if (cross(s.vertices.at(outer.vertices[i]), s.vertices.at(outer.vertices[(i + 1) % outer.vertices.size()]), s.vertices.at(v)) <
              -1e-6 * std::hypot(
                          s.vertices.at(outer.vertices[i]).x - s.vertices.at(outer.vertices[(i + 1) % outer.vertices.size()]).x,
                          s.vertices.at(outer.vertices[i]).z - s.vertices.at(outer.vertices[(i + 1) % outer.vertices.size()]).z))
            return fail(e, "child leaves parent");
    }
    for (size_t i = 0; i < p.vertices.size(); ++i)
    {
      auto a = p.vertices[i], b = p.vertices[(i + 1) % p.vertices.size()];
      if (++edges[{p.parent, a, b}] > 1)
        return fail(e, "shared edge has duplicate orientation");
      auto u = s.vertices.at(a), v = s.vertices.at(b);
      for (auto c : p.vertices)
        if (c != a && c != b && cross(u, v, s.vertices.at(c)) < -1e-6 * std::hypot(v.x - u.x, v.z - u.z))
          return fail(e, "patch must remain convex CCW");
    }
  }
  for (auto a = s.patches.begin(); a != s.patches.end(); ++a)
    for (auto b = s.patches.begin(); b != a; ++b)
      if (a->second.parent == b->second.parent && overlap(s, a->second, b->second))
        return fail(e, "patch interiors overlap");
  auto areaOf = [&](const EditablePatch &p)
  {
    double a = 0;
    for (size_t i = 0; i < p.vertices.size(); ++i)
    {
      auto u = s.vertices.at(p.vertices[i]), v = s.vertices.at(p.vertices[(i + 1) % p.vertices.size()]);
      a += u.x * v.z - u.z * v.x;
    }
    return a * .5;
  };
  std::map<uint64_t, double> childArea;
  for (auto &[id, p] : s.patches)
    if (p.parent)
      childArea[p.parent] += areaOf(p);
  for (auto &[id, a] : childArea)
    if (std::abs(a - areaOf(s.patches.at(id))) > std::max(1e-5, a * 1e-8))
      return fail(e, "children do not tile parent");
  if (e)
    e->clear();
  return true;
}
bool movePatchVertex(const EditablePatchSet &s, uint64_t vertex, Position p, uint64_t expected, EditablePatchSet &out, PatchEditImpact &impact, std::string *e)
{
  if (expected != s.revision || expected == UINT64_MAX || !s.vertices.count(vertex))
    return fail(e, "stale vertex edit or missing vertex");
  if (!validateEditablePatches(s, e))
    return false;
  EditablePatchSet next = s;
  next.vertices.at(vertex) = p;
  next.revision++;
  // Propagate through parent cages in hierarchy order. Shared boundary points
  // map identically from either incident parent; coordinates remain single-owned.
  std::vector<uint64_t> order;
  for (auto &[id, patch] : s.patches)
    order.push_back(id);
  auto depth = [&](uint64_t id)
  {
    size_t d = 0;
    while (s.patches.at(id).parent)
    {
      ++d;
      id = s.patches.at(id).parent;
    }
    return d;
  };
  std::sort(
      order.begin(),
      order.end(),
      [&](auto a, auto b)
      {
        return std::make_pair(depth(a), a) < std::make_pair(depth(b), b);
      });
  std::map<uint64_t, Position> deformed{{vertex, p}};
  for (auto parentId : order)
  {
    const auto &parent = s.patches.at(parentId);
    bool moved = false;
    for (auto v : parent.vertices)
    {
      auto a = s.vertices.at(v), b = next.vertices.at(v);
      moved |= a.x != b.x || a.z != b.z;
    }
    if (!moved)
      continue;
    for (auto &[childId, child] : s.patches)
      if (child.parent == parentId)
        for (auto v : child.vertices)
        {
          if (std::find(parent.vertices.begin(), parent.vertices.end(), v) != parent.vertices.end())
            continue;
          auto q = s.vertices.at(v);
          bool found = false;
          Position mapped;
          for (size_t i = 1; i + 1 < parent.vertices.size(); ++i)
          {
            auto ia = parent.vertices[0], ib = parent.vertices[i], ic = parent.vertices[i + 1];
            auto a = s.vertices.at(ia), b = s.vertices.at(ib), c = s.vertices.at(ic);
            double determinant = cross(a, b, c);
            if (determinant <= 1e-10)
              continue;
            double wa = cross(q, b, c) / determinant, wb = cross(a, q, c) / determinant, wc = 1 - wa - wb;
            if (std::min({wa, wb, wc}) < -1e-8)
              continue;
            auto na = next.vertices.at(ia), nb = next.vertices.at(ib), nc = next.vertices.at(ic);
            mapped = {wa * na.x + wb * nb.x + wc * nc.x, q.y, wa * na.z + wb * nb.z + wc * nc.z};
            found = true;
            break;
          }
          if (!found)
            return fail(e, "cannot bind descendant to parent cage");
          if (deformed.count(v))
          {
            auto prior = deformed.at(v);
            if (std::hypot(prior.x - mapped.x, prior.z - mapped.z) > 1e-5)
              return fail(e, "inconsistent shared descendant deformation");
          }
          else
          {
            deformed.emplace(v, mapped);
            next.vertices.at(v) = mapped;
          }
        }
  }
  PatchEditImpact delta;
  delta.envelope = {INFINITY, INFINITY, -INFINITY, -INFINITY};
  for (auto &[id, patch] : next.patches)
  {
    bool moved = false;
    for (auto v : patch.vertices)
    {
      auto a = s.vertices.at(v), b = next.vertices.at(v);
      moved |= a.x != b.x || a.y != b.y || a.z != b.z;
    }
    if (!moved)
      continue;
    if (patch.revision == UINT64_MAX)
      return fail(e, "patch revision exhausted");
    if (patch.occupied)
      return fail(e, "occupied patch needs deformation transaction");
    ++patch.revision;
    delta.patches.push_back(id);
    expand(delta.envelope, bounds(s, s.patches.at(id)));
    expand(delta.envelope, bounds(next, patch));
  }
  if (delta.patches.empty())
    return fail(e, "vertex has no incident patch");
  if (!validateEditablePatches(next, e))
    return false;
  out = std::move(next);
  impact = std::move(delta);
  return true;
}
bool assignPatchLandUse(const EditablePatchSet &s, uint64_t id, PatchLandUse use, uint64_t expected, EditablePatchSet &out, PatchEditImpact &impact, std::string *e)
{
  if (expected != s.revision || expected == UINT64_MAX || !s.patches.count(id))
    return fail(e, "stale assignment or missing patch");
  if (!validateEditablePatches(s, e))
    return false;
  auto &old = s.patches.at(id);
  if (old.occupied || old.use == PatchLandUse::Protected || old.revision == UINT64_MAX)
    return fail(e, "assignment needs explicit protected/occupied policy");
  if (use < PatchLandUse::Unassigned || use > PatchLandUse::Protected)
    return fail(e, "invalid land use");
  auto next = s;
  next.revision++;
  next.patches.at(id).use = use;
  next.patches.at(id).revision++;
  PatchEditImpact delta{{id}, bounds(s, old)};
  out = std::move(next);
  impact = std::move(delta);
  if (e)
    e->clear();
  return true;
}
std::vector<std::pair<std::string, uint64_t>> patchSpatialReaders(const std::vector<PatchSpatialRead> &reads, const PatchEditImpact &impact)
{
  std::set<std::pair<std::string, uint64_t>> result;
  for (auto &r : reads)
  {
    auto a = r.bounds, b = impact.envelope;
    if (a.maxX >= b.minX && b.maxX >= a.minX && a.maxZ >= b.minZ && b.maxZ >= a.minZ)
      result.emplace(r.invocation, r.domain);
  }
  return {result.begin(), result.end()};
}
} // namespace procedural::world::rewrite
