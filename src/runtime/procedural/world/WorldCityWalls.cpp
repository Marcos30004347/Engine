#include "WorldCityWalls.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
using Poly = std::vector<Position>;
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
    if ((q[i].z > p.z) != (q[j].z > p.z) && p.x < (q[j].x - q[i].x) * (p.z - q[i].z) / (q[j].z - q[i].z) + q[i].x)
      yes = !yes;
  return yes;
}
double segmentDistance(Position p, Position a, Position b)
{
  double l = distance(a, b);
  return distance(p, mix(a, b, std::clamp(((p.x - a.x) * (b.x - a.x) + (p.z - a.z) * (b.z - a.z)) / (l * l), 0., 1.)));
}
Poly band(Position a, Position b, double width)
{
  double l = distance(a, b), x = -(b.z - a.z) * width / l, z = (b.x - a.x) * width / l;
  return {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}};
}
bool overlap(const Poly &a, const Poly &b)
{
  for (auto p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      const auto &other = p == &a ? b : a;
      bool separated = true;
      for (auto q : other)
        if (cross(u, v, q) > 1e-7)
        {
          separated = false;
          break;
        }
      if (separated)
        return false;
    }
  return true;
}

} // namespace
bool generateCityWalls(
    const CityMasterPlan &plan,
    const std::vector<CityPlanCell> &cells,
    const std::vector<CityWallEra> &input,
    const std::vector<CityWallRoute> &inputRoads,
    const CityWallSettings &s,
    const std::function<double(double, double)> &terrain,
    CityWalls &out,
    std::string *error)
{
  auto fail = [&](std::string e)
  {
    if (error)
      *error = std::move(e);
    return false;
  };
  if (!plan.revision || !terrain || cells.empty() || cells.size() > 512 || input.empty() || input.size() > 8 || inputRoads.empty() || inputRoads.size() > 128)
    return fail("invalid wall input budget");
  for (double x : {s.thickness, s.height, s.gateClearance, s.minimumPier, s.maximumFoundationRelief})
    if (!std::isfinite(x) || x <= 0 || x > 100)
      return fail("invalid wall dimensions");
  std::map<uint64_t, const CityPlanCell *> lookup;
  for (auto &c : cells)
    if (!c.id || !lookup.emplace(c.id, &c).second)
      return fail("duplicate wall cell");
  for (const auto &c : cells)
  {
    if (c.boundary.size() < 3 || c.boundary.size() > 32)
      return fail("wall cell polygon budget");
    for (auto p : c.boundary)
      if (!std::isfinite(p.x) || !std::isfinite(p.z) || std::abs(p.x) > 1e6 || std::abs(p.z) > 1e6)
        return fail("invalid wall cell coordinate");
  }
  std::map<uint64_t, const CityPlanDistrict *> districts;
  for (auto &d : plan.districts)
    if (!d.id || !districts.emplace(d.id, &d).second)
      return fail("duplicate wall district");
  auto eras = input;
  auto roads = inputRoads;
  std::sort(
      eras.begin(),
      eras.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  std::sort(
      roads.begin(),
      roads.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  for (size_t i = 0; i < roads.size(); ++i)
  {
    auto &r = roads[i];
    if (!r.id || (i && roads[i - 1].id == r.id) || r.path.size() < 2 || r.path.size() > 256 || !std::isfinite(r.halfWidth) || r.halfWidth <= 0 || r.halfWidth > 20)
      return fail("invalid wall route");
    for (auto p : r.path)
      if (!std::isfinite(p.x) || !std::isfinite(p.z))
        return fail("nonfinite route");
    for (size_t j = 1; j < r.path.size(); ++j)
      if (distance(r.path[j - 1], r.path[j]) < 1e-6)
        return fail("duplicate route point");
  }
  CityWalls result;
  result.revision = plan.revision;
  std::set<uint64_t> defended;
  uint64_t contourID = 1, gateID = 1;
  for (size_t eraIndex = 0; eraIndex < eras.size(); ++eraIndex)
  {
    const auto &era = eras[eraIndex];
    if (!era.id || (eraIndex && era.id == eras[eraIndex - 1].id))
      return fail("duplicate wall era");
    for (auto id : era.districts)
    {
      if (!districts.count(id))
        return fail("unknown defended district");
      for (auto cell : districts.at(id)->cells)
        defended.insert(cell);
    }
    defended.insert(era.infillCells.begin(), era.infillCells.end());
    if (defended.empty())
      return fail("empty wall era");
    std::vector<SurfaceContour> contours;
    SurfaceMapSettings bounds;
    bounds.minX = bounds.minZ = 1e30;
    bounds.maxX = bounds.maxZ = -1e30;
    for (auto id : defended)
    {
      if (!lookup.count(id))
        return fail("unknown defended cell");
      auto &c = *lookup.at(id);
      if (!c.buildable)
        return fail("wall encloses excluded cell");
      SurfaceContour contour;
      contour.id = id;
      contour.material = 1;
      contour.loop = c.boundary;
      contours.push_back(contour);
      for (auto p : c.boundary)
      {
        bounds.minX = std::min(bounds.minX, p.x - 10);
        bounds.maxX = std::max(bounds.maxX, p.x + 10);
        bounds.minZ = std::min(bounds.minZ, p.z - 10);
        bounds.maxZ = std::max(bounds.maxZ, p.z + 10);
      }
    }
    if (bounds.maxX - bounds.minX > 4000 || bounds.maxZ - bounds.minZ > 4000)
      return fail("wall site exceeds bounded extent");
    SurfaceArrangement map;
    std::string why;
    if (!arrangeSurfaceContours(
            contours,
            bounds,
            [](double, double)
            {
              return 0.;
            },
            plan.revision,
            map,
            &why))
      return fail("wall union: " + why);
    std::map<uint32_t, uint32_t> next;
    std::map<uint32_t, unsigned> incoming;
    for (auto &e : map.edges)
    {
      bool left = e.leftFace && map.faces[e.leftFace - 1].material == 1, right = e.rightFace && map.faces[e.rightFace - 1].material == 1;
      if (left == right)
        continue;
      auto a = left ? e.a : e.b, b = left ? e.b : e.a;
      if (!next.emplace(a, b).second || ++incoming[b] > 1)
        return fail("wall union branches at point contact");
    }
    if (next.empty())
      return fail("wall union is empty");
    Poly loop;
    auto start = next.begin()->first, v = start;
    std::set<uint32_t> visited;
    do
    {
      if (!next.count(v) || !visited.insert(v).second)
        return fail("wall union is not a simple circuit");
      loop.push_back(map.vertices[v]);
      v = next.at(v);
    } while (v != start);
    if (visited.size() != next.size() || area(loop) <= 0)
      return fail("wall era has disconnected components or holes");
    // Dissolve arrangement helper-cut vertices without moving authored corners.
    bool changed = true;
    while (changed && loop.size() > 3)
    {
      changed = false;
      for (size_t i = 0; i < loop.size(); ++i)
        if (std::abs(cross(loop[(i + loop.size() - 1) % loop.size()], loop[i], loop[(i + 1) % loop.size()])) < 1e-7)
        {
          loop.erase(loop.begin() + i);
          changed = true;
          break;
        }
    }
    if (loop.size() > 256)
      return fail("wall boundary budget exceeded");
    if (!result.circuits.empty())
      for (auto p : result.circuits.back().boundary)
      {
        if (!inside(p, loop))
          return fail("new wall must strictly enclose previous circuit");
        for (size_t j = 0; j < loop.size(); ++j)
          if (segmentDistance(p, loop[j], loop[(j + 1) % loop.size()]) < s.thickness + s.minimumPier)
            return fail("wall circuits lack clearance");
      }
    // Check both directions of edge clearance, including concave tips between
    // old vertices. Corner-only containment is insufficient for concave outlines.
    auto separated = [&](const Poly &a, const Poly &b, bool same)
    {
      for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
        {
          if (same && (i == j || (i + 1) % a.size() == j || (j + 1) % a.size() == i))
            continue;
          auto p = a[i], q = a[(i + 1) % a.size()], r = b[j], t = b[(j + 1) % b.size()];
          if (cross(p, q, r) * cross(p, q, t) < 0 && cross(r, t, p) * cross(r, t, q) < 0)
            return false;
          if (std::min({segmentDistance(p, r, t), segmentDistance(q, r, t), segmentDistance(r, p, q), segmentDistance(t, p, q)}) < s.thickness + s.minimumPier)
            return false;
        }
      return true;
    };
    if (!separated(loop, loop, true))
      return fail("wall footprint has an unsupported narrow neck");
    if (!result.circuits.empty() && !separated(loop, result.circuits.back().boundary, false))
      return fail("historical wall edges lack clearance");
    CityWallCircuit circuit;
    circuit.era = era.id;
    circuit.boundary = loop;
    auto publishPolygon = [&](Poly footprint)
    {
      if (area(footprint) < 0)
        std::reverse(footprint.begin(), footprint.end());
      if (area(footprint) < 1e-8)
        return true;
      double low = 1e30, high = -1e30;
      for (size_t j = 0; j < footprint.size(); ++j)
      {
        auto a = footprint[j], b = footprint[(j + 1) % footprint.size()];
        double ha = terrain(a.x, a.z), hb = terrain(b.x, b.z);
        unsigned samples = std::max(1u, unsigned(std::ceil(distance(a, b) / 2)));
        for (unsigned k = 0; k <= samples; ++k)
        {
          double t = double(k) / samples;
          auto p = mix(a, b, t);
          double h = terrain(p.x, p.z);
          if (!std::isfinite(h) || std::abs(h - (ha + (hb - ha) * t)) > 1e-4)
            return false;
          low = std::min(low, h);
          high = std::max(high, h);
        }
      }
      if (high - low > s.maximumFoundationRelief)
        return false;
      if (result.foundations.size() >= 2048)
        return false;
      for (const auto &cell : cells)
        if (!cell.buildable && overlap(footprint, cell.boundary))
          return false;
      SurfaceContour foundation;
      foundation.id = contourID++;
      foundation.material = 11;
      foundation.loop = footprint;
      result.foundations.push_back(foundation);
      Poly bottom = footprint, top = footprint;
      for (size_t j = 0; j < footprint.size(); ++j)
      {
        bottom[j].y = terrain(bottom[j].x, bottom[j].z);
        top[j].y = high + s.height;
      }
      auto tri = [&](Position p, Position q, Position r)
      {
        result.triangles.push_back({{p, q, r}, era.id});
      };
      for (size_t j = 0; j < footprint.size(); ++j)
      {
        size_t k = (j + 1) % footprint.size();
        tri(bottom[j], top[j], top[k]);
        tri(bottom[j], top[k], bottom[k]);
      }
      for (size_t j = 1; j + 1 < footprint.size(); ++j)
        tri(top[0], top[j + 1], top[j]);
      return true;
    };
    // Beveled outside joins close the corner gaps between butt-ended wall strips.
    for (size_t j = 0; j < loop.size(); ++j)
    {
      auto p = loop[j], a = loop[(j + loop.size() - 1) % loop.size()], b = loop[(j + 1) % loop.size()];
      double la = distance(a, p), lb = distance(p, b), side = cross(a, p, b) > 0 ? 1 : -1;
      Position u{p.x + side * (p.z - a.z) / la * s.thickness * .5, 0, p.z - side * (p.x - a.x) / la * s.thickness * .5};
      Position v{p.x + side * (b.z - p.z) / lb * s.thickness * .5, 0, p.z - side * (b.x - p.x) / lb * s.thickness * .5};
      if (!publishPolygon({p, u, v}))
        return fail("wall corner needs foundation engineering");
    }
    size_t gateStart = result.gates.size();
    for (size_t edge = 0; edge < loop.size(); ++edge)
    {
      auto a = loop[edge], b = loop[(edge + 1) % loop.size()];
      double len = distance(a, b);
      circuit.perimeter += len;
      struct Cut
      {
        double lo, hi;
        std::set<uint64_t> roads;
      };
      std::vector<Cut> cuts;
      for (auto &r : roads)
        for (size_t k = 1; k < r.path.size(); ++k)
        {
          auto c = r.path[k - 1], d = r.path[k];
          double dx = d.x - c.x, dz = d.z - c.z, ex = b.x - a.x, ez = b.z - a.z, den = ex * dz - ez * dx;
          if (std::abs(den) < 1e-9)
          {
            if (std::min(segmentDistance(c, a, b), segmentDistance(d, a, b)) < r.halfWidth + s.thickness * .5)
              return fail("route follows wall too closely; reroute before wall generation");
            continue;
          }
          double t = ((c.x - a.x) * dz - (c.z - a.z) * dx) / den, u = ((c.x - a.x) * ez - (c.z - a.z) * ex) / den;
          if (t < -1e-8 || t > 1 + 1e-8 || u < -1e-8 || u > 1 + 1e-8)
            continue;
          // A route ending at a wall is not a traversing access connection.
          // Interior polyline vertices on the wall use the neighbors on both sides.
          auto before = c, after = d;
          if (u < 1e-8)
          {
            if (k < 2)
              continue;
            before = r.path[k - 2];
          }
          if (u > 1 - 1e-8)
          {
            if (k + 1 >= r.path.size())
              continue;
            after = r.path[k + 1];
          }
          if (cross(a, b, before) * cross(a, b, after) >= -1e-8)
            continue;
          double sinAngle = std::abs(den) / (len * distance(c, d));
          double half = (r.halfWidth + s.gateClearance + s.thickness * .5 * std::sqrt(std::max(0., 1 - sinAngle * sinAngle))) / sinAngle;
          double lo = t * len - half, hi = t * len + half;
          if (lo < s.minimumPier || hi > len - s.minimumPier)
            return fail("gate too close to wall corner; route needs relocation");
          cuts.push_back({lo, hi, {r.id}});
        }
      std::sort(
          cuts.begin(),
          cuts.end(),
          [](auto &a, auto &b)
          {
            return a.lo < b.lo;
          });
      std::vector<Cut> merged;
      for (auto c : cuts)
      {
        if (!merged.empty() && c.lo - merged.back().hi < s.minimumPier)
        {
          merged.back().hi = std::max(merged.back().hi, c.hi);
          merged.back().roads.insert(c.roads.begin(), c.roads.end());
        }
        else
          merged.push_back(c);
      }
      auto publish = [&](double lo, double hi)
      {
        if (hi - lo < 1e-7)
          return true;
        return publishPolygon(band(mix(a, b, lo / len), mix(a, b, hi / len), s.thickness * .5));
      };
      double cursor = 0;
      for (auto &c : merged)
      {
        if (!publish(cursor, c.lo))
          return fail("wall foundation relief needs engineering");
        CityWallGate gate;
        gate.id = gateID++;
        gate.era = era.id;
        gate.a = mix(a, b, c.lo / len);
        gate.b = mix(a, b, c.hi / len);
        gate.routes.assign(c.roads.begin(), c.roads.end());
        result.gates.push_back(gate);
        circuit.openingLength += c.hi - c.lo;
        cursor = c.hi;
      }
      if (!publish(cursor, len))
        return fail("wall foundation relief needs engineering");
    }
    if (result.gates.size() == gateStart)
      return fail("wall circuit has no access gate");
    result.circuits.push_back(std::move(circuit));
  }
  // No road-width envelope may collide with any published solid wall, including
  // non-crossing approaches and corner joins. Unsupported cases fail for rerouting.
  for (const auto &r : roads)
    for (size_t k = 1; k < r.path.size(); ++k)
    {
      double radius = r.halfWidth + s.gateClearance, length = distance(r.path[k - 1], r.path[k]);
      // Square-ended envelope conservatively includes round caps and polyline joins.
      auto envelope = band(mix(r.path[k - 1], r.path[k], -radius / length), mix(r.path[k - 1], r.path[k], 1 + radius / length), radius);
      for (const auto &f : result.foundations)
        if (overlap(envelope, f.loop))
          return fail("road clearance intersects solid wall; reroute or widen gate");
    }
  out = std::move(result);
  if (error)
    error->clear();
  return true;
}
} // namespace procedural::world::rewrite
