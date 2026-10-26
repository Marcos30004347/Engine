#include "WorldCityBuildingRegions.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
using Shape = CityRegionPolygon;
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
double distance(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}
double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
bool inside(Position p, const Poly &q)
{
  bool yes = false;
  for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++)
  {
    auto a = q[j], b = q[i];
    if (std::abs(cross(a, b, p)) < 1e-7 && p.x >= std::min(a.x, b.x) - 1e-7 && p.x <= std::max(a.x, b.x) + 1e-7 && p.z >= std::min(a.z, b.z) - 1e-7 && p.z <= std::max(a.z, b.z) + 1e-7)
      return true;
    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
      yes = !yes;
  }
  return yes;
}
std::vector<Poly> rings(const Shape &s)
{
  auto r = s.holes;
  r.insert(r.begin(), s.outer);
  return r;
}
bool contained(const Poly &p, const Shape &s)
{
  for (auto v : p)
  {
    if (!inside(v, s.outer))
      return false;
    for (auto &h : s.holes)
      if (inside(v, h))
        return false;
  }
  for (auto &q : rings(s))
    for (size_t i = 0; i < p.size(); ++i)
      for (size_t j = 0; j < q.size(); ++j)
      {
        auto a = p[i], b = p[(i + 1) % p.size()], c = q[j], d = q[(j + 1) % q.size()];
        if (cross(a, b, c) * cross(a, b, d) < -1e-10 && cross(c, d, a) * cross(c, d, b) < -1e-10)
          return false;
      }
  for (auto &h : s.holes)
    if (inside(h.front(), p))
      return false;
  return true;
}
Poly band(Position a, Position b, double width, bool caps)
{
  double len = distance(a, b);
  auto u = caps ? mix(a, b, -width / len) : a, v = caps ? mix(a, b, 1 + width / len) : b;
  double x = -(b.z - a.z) / len * width, z = (b.x - a.x) / len * width;
  return {{u.x - x, 0, u.z - z}, {v.x - x, 0, v.z - z}, {v.x + x, 0, v.z + z}, {u.x + x, 0, u.z + z}};
}
bool patch(const Shape &s, double height, PatchMesh &mesh, std::string *error)
{
  BoundaryRegistry registry;
  PatchDomain domain;
  domain.id = 1;
  std::vector<BoundaryInput> edges;
  uint64_t vid = 1, eid = 1;
  auto add = [&](const Poly &p, std::vector<BoundaryUse> &uses)
  {
    uint64_t first = vid;
    for (size_t i = 0; i < p.size(); ++i)
    {
      auto a = p[i], b = p[(i + 1) % p.size()];
      a.y = b.y = height;
      edges.push_back({eid, {vid++, a}, {i + 1 == p.size() ? first : vid, b}, {}});
      uses.push_back({eid++, i + 1 == p.size()});
    }
  };
  add(s.outer, domain.outer);
  for (auto &h : s.holes)
  {
    domain.holes.emplace_back();
    add(h, domain.holes.back());
  }
  return registry.commit(edges, error) && compilePatchMesh(
                                              domain,
                                              registry,
                                              {},
                                              [=](double, double)
                                              {
                                                return height;
                                              },
                                              mesh,
                                              error);
}
bool extract(const SurfaceArrangement &m, uint32_t material, std::vector<Shape> &shapes, std::string *error, size_t maximumRingVertices = 128, bool splitPointContacts = false)
{
  std::map<uint32_t, std::vector<uint32_t>> next;
  std::map<uint32_t, unsigned> incoming;
  for (auto &e : m.edges)
    if (e.layer == 0)
    {
      if (e.a >= m.vertices.size() || e.b >= m.vertices.size() || e.leftFace > m.faces.size() || e.rightFace > m.faces.size())
      {
        if (error)
          *error = "invalid map edge";
        return false;
      }
      bool left = e.leftFace && m.faces[e.leftFace - 1].material == material, right = e.rightFace && m.faces[e.rightFace - 1].material == material;
      if (left == right)
        continue;
      auto a = left ? e.a : e.b, b = left ? e.b : e.a;
      next[a].push_back(b);
      ++incoming[b];
      if (!splitPointContacts && (next[a].size() > 1 || incoming[b] > 1))
      {
        if (error)
          *error = "point-touching block requires arrangement repair";
        return false;
      }
    }
  std::vector<Poly> holes;
  while (!next.empty())
  {
    Poly p;
    auto start = next.begin()->first, v = start;
    uint32_t previous = UINT32_MAX;
    do
    {
      auto it = next.find(v);
      if (it == next.end())
      {
        if (error)
          *error = "open block ring";
        return false;
      }
      p.push_back(m.vertices[v]);
      size_t chosen = 0;
      if (previous != UINT32_MAX && it->second.size() > 1)
      {
        // Material stays to the left: choose the boundary ray immediately
        // clockwise from the incoming reverse ray. Point contacts become
        // separate rings instead of connecting unrelated blocks through a point.
        const auto center = m.vertices[v], from = m.vertices[previous];
        const double reverse = std::atan2(from.z - center.z, from.x - center.x);
        double best = 1e30;
        for (size_t k = 0; k < it->second.size(); ++k)
        {
          const auto to = m.vertices[it->second[k]];
          double turn = std::fmod(reverse - std::atan2(to.z - center.z, to.x - center.x) + 6.283185307179586, 6.283185307179586);
          if (turn < best)
          {
            chosen = k;
            best = turn;
          }
        }
      }
      previous = v;
      v = it->second[chosen];
      it->second.erase(it->second.begin() + chosen);
      if (it->second.empty())
        next.erase(it);
    } while (v != start);
    bool changed = true;
    while (changed && p.size() > 3)
    {
      changed = false;
      for (size_t i = 0; i < p.size(); ++i)
        if (std::abs(cross(p[(i + p.size() - 1) % p.size()], p[i], p[(i + 1) % p.size()])) < 1e-7)
        {
          p.erase(p.begin() + i);
          changed = true;
          break;
        }
    }
    if (p.size() > maximumRingVertices)
    {
      if (error)
        *error = "block ring vertex budget exceeded";
      return false;
    }
    if (area(p) > 1e-6)
      shapes.push_back({p, {}});
    else if (area(p) < -1e-6)
      holes.push_back(p);
  }
  for (auto &h : holes)
  {
    bool assigned = false;
    for (auto &s : shapes)
      if (inside(h.front(), s.outer))
      {
        s.holes.push_back(h);
        assigned = true;
        break;
      }
    if (!assigned)
    {
      if (error)
        *error = "unowned block hole";
      return false;
    }
  }
  return true;
}
bool masks(
    const std::vector<Shape> &base,
    const std::vector<Shape> &subtract,
    const std::vector<Poly> &cuts,
    double erosion,
    const SurfaceMapSettings &bounds,
    uint64_t revision,
    std::vector<Shape> &out,
    std::string *error)
{
  std::vector<SurfaceContour> contours;
  uint64_t id = 1;
  auto add = [&](const Shape &s, uint32_t material)
  {
    PatchMesh mesh;
    if (!patch(s, 0, mesh, error))
      return false;
    for (auto &t : mesh.triangles)
    {
      SurfaceContour c;
      c.id = id++;
      c.material = material;
      for (auto i : t)
        c.loop.push_back(mesh.vertices[i].position);
      if (area(c.loop) < 0)
        std::reverse(c.loop.begin(), c.loop.end());
      contours.push_back(c);
    }
    return true;
  };
  for (auto &s : base)
    if (!add(s, 1))
      return false;
  for (auto &s : subtract)
    if (!add(s, 0))
      return false;
  auto cut = [&](Poly p)
  {
    SurfaceContour c;
    c.id = id++;
    c.material = 0;
    c.loop = std::move(p);
    contours.push_back(std::move(c));
  };
  for (auto p : cuts)
    cut(p);
  if (erosion > 0)
    for (auto &s : base)
      for (auto &p : rings(s))
        for (size_t i = 0; i < p.size(); ++i)
        {
          cut(band(p[i], p[(i + 1) % p.size()], erosion, false));
          // Circumscribed round caps preserve the clearance guarantee without
          // the rectangular notches produced by square caps on curved frontages.
          Poly cap;
          constexpr unsigned segments = 16;
          double radius = erosion / std::cos(3.141592653589793 / segments);
          for (unsigned k = 0; k < segments; ++k)
          {
            double angle = 2 * 3.141592653589793 * k / segments;
            cap.push_back({p[i].x + radius * std::cos(angle), 0, p[i].z + radius * std::sin(angle)});
          }
          cut(cap);
        }
  SurfaceArrangement map;
  return arrangeSurfaceContours(
             contours,
             bounds,
             [](double, double)
             {
               return 0.;
             },
             revision,
             map,
             error) &&
         extract(map, 1, out, error);
}
} // namespace
bool extractCityStreetBlocks(const SurfaceArrangement &map, uint32_t material, std::vector<CityStreetBlock> &out, std::string *error)
{
  // Rounded city outlines retain more collinear constraints; allow up to three
  // edges per bounded face while preserving the existing face/vertex limits.
  if (!map.revision || map.vertices.size() > 50000 || map.edges.size() > 60000 || map.faces.size() > 20000)
  {
    if (error)
      *error = "invalid street block extraction budget (vertices=" + std::to_string(map.vertices.size()) + ", edges=" + std::to_string(map.edges.size()) + ", faces=" + std::to_string(map.faces.size()) + ")";
    return false;
  }
  for (const auto &v : map.vertices)
    if (!std::isfinite(v.x) || !std::isfinite(v.z))
    {
      if (error)
        *error = "nonfinite street block vertex";
      return false;
    }
  std::vector<Shape> shapes;
  if (!extract(map, material, shapes, error, 2048, true))
    return false;
  std::vector<CityStreetBlock> blocks;
  for (auto &shape : shapes)
    blocks.push_back({blocks.size() + 1, std::move(shape), {}, {}});
  out = std::move(blocks);
  return true;
}
bool generateCityBuildingRegions(const SurfaceArrangement &map, const CityBuildingRegionSettings &s, const std::function<double(double, double)> &terrain, CityBuildingRegions &out, std::string *error)
{
  auto fail = [&](std::string e)
  {
    if (error)
      *error = std::move(e);
    return false;
  };
  if (!map.revision || !terrain || !s.district || unsigned(s.use) >= unsigned(DistrictUse::Count) || map.faces.size() > 20000 || map.edges.size() > 40000)
    return fail("invalid region input budget");
  if (map.faces.empty() || map.vertices.size() > 50000 || !std::isfinite(map.settings.minX) || !std::isfinite(map.settings.minZ) || !std::isfinite(map.settings.maxX) || !std::isfinite(map.settings.maxZ) ||
      map.settings.maxX <= map.settings.minX || map.settings.maxZ <= map.settings.minZ || map.settings.maxX - map.settings.minX > 4000 || map.settings.maxZ - map.settings.minZ > 4000)
    return fail("invalid building map extent");
  for (const auto &f : map.faces)
    if (f.material == s.landMaterial && f.layer == 0 && f.role != SurfaceRole::Ground)
      return fail("buildable material includes a protected surface role");
  for (double x : {s.setback, s.depth, s.passageWidth, s.height, s.minimumArea, s.minimumCourtyardArea, s.maximumRelief})
    if (!std::isfinite(x) || x <= 0 || x > 10000)
      return fail("invalid region dimensions");
  if (s.depth < 3 || s.passageWidth < 2 || s.depth > 50 || s.setback > 20 || s.passageWidth > 12)
    return fail("unsupported wing or passage dimensions");
  for (size_t i = 0; i < map.faces.size(); ++i)
    if (map.faces[i].id != i + 1)
      return fail("noncanonical map face IDs");
  for (auto p : map.vertices)
    if (!std::isfinite(p.x) || !std::isfinite(p.z))
      return fail("nonfinite region map vertex");
  std::vector<Shape> blocks;
  if (!extract(map, s.landMaterial, blocks, error))
    return false;
  if (blocks.size() > 64)
    return fail("building block budget exceeded");
  CityBuildingRegions result;
  result.revision = map.revision;
  uint64_t blockID = 1, regionID = 1, contourID = 1;
  for (auto &shape : blocks)
  {
    CityStreetBlock block;
    block.id = blockID++;
    block.boundary = shape;
    SurfaceMapSettings bounds = map.settings;
    double pad = 2 * (s.setback + s.depth) + 5;
    for (auto &p : rings(shape))
      for (auto v : p)
      {
        bounds.minX = std::min(bounds.minX, v.x - pad);
        bounds.maxX = std::max(bounds.maxX, v.x + pad);
        bounds.minZ = std::min(bounds.minZ, v.z - pad);
        bounds.maxZ = std::max(bounds.maxZ, v.z + pad);
      }
    std::vector<Shape> buildable, courts;
    if (!masks({shape}, {}, {}, s.setback, bounds, map.revision, buildable, error) || !masks({shape}, {}, {}, s.setback + s.depth, bounds, map.revision, courts, error))
      return false;
    courts.erase(
        std::remove_if(
            courts.begin(),
            courts.end(),
            [&](auto &c)
            {
              double a = area(c.outer);
              for (auto &h : c.holes)
                a -= std::abs(area(h));
              return a < s.minimumCourtyardArea;
            }),
        courts.end());
    block.courtyards = courts;
    if (courts.size() > 1)
    {
      block.rejection = "multiple courtyards require additional access planning";
      result.blocks.push_back(std::move(block));
      continue;
    }
    if (courts.empty())
    {
      block.rejection = "insufficient courtyard width/area";
      result.blocks.push_back(std::move(block));
      continue;
    }
    struct Frontage
    {
      uint64_t edge;
      Position a, b;
    };
    std::vector<Frontage> frontages;
    for (auto &e : map.edges)
      if (e.layer == 0 && e.leftFace && e.rightFace)
      {
        auto &l = map.faces[e.leftFace - 1], &r = map.faces[e.rightFace - 1];
        bool left = l.material == s.landMaterial && (r.role == SurfaceRole::Road || r.role == SurfaceRole::Shoulder),
             right = r.material == s.landMaterial && (l.role == SurfaceRole::Road || l.role == SurfaceRole::Shoulder);
        if (!left && !right)
          continue;
        auto a = map.vertices[left ? e.a : e.b], b = map.vertices[left ? e.b : e.a];
        auto mid = mix(a, b, .5);
        if (inside(mid, shape.outer) && distance(a, b) > s.passageWidth + 2)
          frontages.push_back({e.id, a, b});
      }
    std::sort(
        frontages.begin(),
        frontages.end(),
        [](auto &a, auto &b)
        {
          double la = distance(a.a, a.b), lb = distance(b.a, b.b);
          return la == lb ? a.edge < b.edge : la > lb;
        });
    Poly passage;
    Frontage chosen{};
    Position streetAccess;
    for (auto &f : frontages)
    {
      for (double fraction : {.5, .25, .75})
      {
        auto origin = mix(f.a, f.b, fraction);
        double len = distance(f.a, f.b), nx = -(f.b.z - f.a.z) / len, nz = (f.b.x - f.a.x) / len, best = 1e30;
        for (auto &court : courts)
          for (size_t i = 0; i < court.outer.size(); ++i)
          {
            auto a = court.outer[i], b = court.outer[(i + 1) % court.outer.size()];
            double dx = b.x - a.x, dz = b.z - a.z, den = nx * dz - nz * dx;
            if (std::abs(den) < 1e-9)
              continue;
            double t = ((a.x - origin.x) * dz - (a.z - origin.z) * dx) / den, u = ((a.x - origin.x) * nz - (a.z - origin.z) * nx) / den;
            if (t > 0 && u >= 0 && u <= 1)
              best = std::min(best, t);
          }
        if (best == 1e30)
          continue;
        Position end{origin.x + nx * (best + .5), 0, origin.z + nz * (best + .5)};
        auto p = band(origin, end, s.passageWidth * .5, false);
        auto courtLip = band({end.x - nx * .1, 0, end.z - nz * .1}, end, s.passageWidth * .5, false);
        if (contained(p, shape) && contained(courtLip, courts.front()))
        {
          passage = p;
          chosen = f;
          streetAccess = origin;
          break;
        }
      }
      if (!passage.empty())
        break;
    }
    if (passage.empty())
    {
      block.rejection = "no clear road-to-courtyard passage";
      result.blocks.push_back(std::move(block));
      continue;
    }
    std::vector<Shape> footprints;
    if (!masks(buildable, courts, {passage}, 0, bounds, map.revision, footprints, error))
      return false;
    for (auto &foot : footprints)
    {
      bool touchesPassage = false;
      for (auto &p : rings(foot))
        for (size_t i = 0; i < p.size(); ++i)
          for (size_t j = 0; j < passage.size(); ++j)
          {
            auto a = p[i], b = p[(i + 1) % p.size()], c = passage[j], d = passage[(j + 1) % passage.size()];
            double len = distance(c, d);
            if (std::abs(cross(c, d, a)) / len > .003 || std::abs(cross(c, d, b)) / len > .003)
              continue;
            auto t = [&](Position v)
            {
              return ((v.x - c.x) * (d.x - c.x) + (v.z - c.z) * (d.z - c.z)) / (len * len);
            };
            if ((std::min(1., std::max(t(a), t(b))) - std::max(0., std::min(t(a), t(b)))) * len > 1)
              touchesPassage = true;
          }
      if (!touchesPassage)
        continue;
      PatchMesh roof;
      if (!patch(foot, 0, roof, error))
        return false;
      if (roof.domainArea < s.minimumArea)
        continue;
      double low = 1e30, high = -1e30;
      bool prepared = true;
      for (auto &p : rings(foot))
        for (size_t i = 0; i < p.size(); ++i)
        {
          auto a = p[i], b = p[(i + 1) % p.size()];
          for (unsigned k = 0, n = std::max(1u, unsigned(std::ceil(distance(a, b) / 2))); k <= n; ++k)
          {
            auto q = mix(a, b, double(k) / n);
            double h = terrain(q.x, q.z);
            if (!std::isfinite(h))
              return fail("nonfinite region terrain");
            prepared &= std::abs(h - (terrain(a.x, a.z) + (terrain(b.x, b.z) - terrain(a.x, a.z)) * double(k) / n)) < 1e-4;
            low = std::min(low, h);
            high = std::max(high, h);
          }
        }
      if (!prepared || high - low > s.maximumRelief)
      {
        block.rejection = "building region requires foundation engineering";
        continue;
      }
      CityBuildingRegion region;
      region.id = regionID++;
      region.block = block.id;
      region.district = s.district;
      region.use = s.use;
      region.footprint = foot;
      region.frontageEdge = chosen.edge;
      region.passage = passage;
      region.streetAccess = streetAccess;
      region.foundation = high;
      region.height = s.height;
      region.area = roof.domainArea;
      for (auto &t : roof.triangles)
      {
        CityRegionTriangle tri;
        tri.region = region.id;
        tri.material = 13;
        SurfaceContour foundation;
        foundation.id = contourID++;
        foundation.material = 12;
        for (size_t k = 0; k < 3; ++k)
        {
          auto p = roof.vertices[t[k]].position;
          foundation.loop.push_back(p);
          p.y = high + s.height;
          tri.points[k] = p;
        }
        std::reverse(foundation.loop.begin(), foundation.loop.end());
        result.foundations.push_back(foundation);
        result.triangles.push_back(tri);
      }
      for (auto &p : rings(foot))
        for (size_t i = 0; i < p.size(); ++i)
        {
          auto a = p[i], b = p[(i + 1) % p.size()], ta = a, tb = b;
          a.y = terrain(a.x, a.z);
          b.y = terrain(b.x, b.z);
          ta.y = tb.y = high + s.height;
          result.triangles.push_back({{a, ta, tb}, region.id, 12});
          result.triangles.push_back({{a, tb, b}, region.id, 12});
        }
      result.regions.push_back(std::move(region));
    }
    if (std::none_of(
            result.regions.begin(),
            result.regions.end(),
            [&](auto &r)
            {
              return r.block == block.id;
            }) &&
        block.rejection.empty())
      block.rejection = "building fragments below minimum area";
    result.blocks.push_back(std::move(block));
  }
  if (result.regions.size() > 256 || result.foundations.size() > 2048)
    return fail("building region output budget exceeded");
  out = std::move(result);
  if (error)
    error->clear();
  return true;
}
} // namespace procedural::world::rewrite
