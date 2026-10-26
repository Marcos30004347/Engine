#include "WorldSettlementAssembly.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace procedural::world::rewrite {
namespace {
double distance(Position p, Position a, Position b)
{
  double x = b.x - a.x, z = b.z - a.z, d = x * x + z * z;
  double t = d ? std::clamp(((p.x - a.x) * x + (p.z - a.z) * z) / d, 0., 1.) : 0;
  return std::hypot(p.x - a.x - t * x, p.z - a.z - t * z);
}

bool intersects(Position a, Position b, Position c, Position d)
{
  auto cross = [](Position p, Position q, Position r) {
    return (q.x - p.x) * (r.z - p.z) - (q.z - p.z) * (r.x - p.x);
  };
  if (std::min({distance(a, c, d), distance(b, c, d), distance(c, a, b), distance(d, a, b)}) < 1e-6)
  {
    return true;
  }
  return cross(a, b, c) * cross(a, b, d) < 0 && cross(c, d, a) * cross(c, d, b) < 0;
}

bool street(SurfaceRole r)
{
  return r == SurfaceRole::Road || r == SurfaceRole::Shoulder;
}
} // namespace

bool assembleSettlement(const SettlementAssemblyInput &in, const RoadField &terrain, SettlementAssembly &out, std::string *error)
{
  auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (in.streets.empty() || in.streets.size() > 64)
  {
    return fail("settlement requires 1..64 streets");
  }
  SettlementAssembly r;
  if (!generateCityMasterPlan(in.cells, in.districts, in.gateCells, in.planning, r.plan, error))
  {
    return false;
  }
  r.roads.dependencyRevision = in.planning.revision;
  std::vector<CityWallRoute> routes;
  for (auto &p : in.streets)
  {
    RoutedRoad road;
    if (!designRoad(
            p,
            {},
            terrain,
            [](double, double) {
              return 0.;
            },
            in.roads,
            road.design,
            error))
    {
      return false;
    }
    CityWallRoute route;
    route.id = routes.size() + 1;
    route.halfWidth = in.roads.halfWidthMetres + in.roads.shoulderMetres;
    for (auto &s : road.design.sections)
    {
      route.path.push_back(s.center);
    }
    routes.push_back(route);
    r.roads.roads.push_back(std::move(road));
  }
  std::vector<size_t> groups(routes.size());
  std::iota(groups.begin(), groups.end(), 0);
  for (size_t i = 0; i < routes.size(); ++i)
  {
    for (size_t j = 0; j < i; ++j)
    {
      for (size_t a = 1; a < routes[i].path.size(); ++a)
      {
        for (size_t b = 1; b < routes[j].path.size(); ++b)
        {
          if (intersects(routes[i].path[a - 1], routes[i].path[a], routes[j].path[b - 1], routes[j].path[b]))
          {
            auto old = groups[i], next = groups[j];
            for (auto &g : groups)
            {
              if (g == old)
              {
                g = next;
              }
            }
          }
        }
      }
    }
  }
  for (auto g : groups)
  {
    if (g != groups[0])
    {
      return fail("disconnected settlement streets");
    }
  }
  auto served = [&](uint64_t id) {
    for (auto &c : in.cells)
    {
      if (c.id == id)
      {
        Position p{};
        for (auto v : c.boundary)
        {
          p.x += v.x / c.boundary.size();
          p.z += v.z / c.boundary.size();
        }
        for (auto &route : routes)
        {
          for (size_t k = 1; k < route.path.size(); ++k)
          {
            if (distance(p, route.path[k - 1], route.path[k]) <= in.roads.halfWidthMetres)
            {
              return true;
            }
          }
        }
      }
    }
    return false;
  };
  for (auto &d : r.plan.districts)
  {
    if (!served(d.anchorCell))
    {
      return fail("district anchor lacks engineered street access");
    }
  }
  for (auto g : in.gateCells)
  {
    if (!served(g))
    {
      return fail("planning gate lacks engineered street access");
    }
  }
  std::vector<SurfaceContour> contours;
  uint64_t id = 1;
  for (size_t i = 0; i < r.plan.districts.size(); ++i)
  {
    for (auto &p : r.plan.districts[i].pieces)
    {
      SurfaceContour c;
      c.id = id++;
      c.material = 20 + i;
      c.loop = p;
      contours.push_back(c);
    }
  }
  RoadEarthworkProduct earth;
  if (!generateRoadEarthworks(
          r.roads,
          terrain,
          [](double, double) {
            return RoadWaterSample{};
          },
          contours,
          {},
          in.earthworks,
          earth,
          error))
  {
    return false;
  }
  contours = earth.contours;
  for (auto &c : contours)
  {
    id = std::max(id, c.id + 1);
  }
  if (!generateCityWalls(r.plan, in.cells, in.eras, routes, in.walls, terrain, r.walls, error))
  {
    return false;
  }
  auto append = [&](std::vector<SurfaceContour> &cs) {
    for (auto &c : cs)
    {
      c.id = id++;
      contours.push_back(c);
    }
  };
  append(r.walls.foundations);
  if (!arrangeSurfaceContours(contours, in.earthworks.map, terrain, in.planning.revision, r.map, error))
  {
    return false;
  }
  r.buildings.revision = in.planning.revision;
  for (size_t i = 0; i < r.plan.districts.size(); ++i)
  {
    auto s = in.buildings;
    s.landMaterial = 20 + i;
    s.district = r.plan.districts[i].id;
    s.use = r.plan.districts[i].use;
    CityBuildingRegions b;
    if (!generateCityBuildingRegions(r.map, s, terrain, b, error))
    {
      return false;
    }
    if (b.regions.empty())
    {
      std::string reason;
      for (auto &block : b.blocks)
      {
        reason += " / " + block.rejection;
      }
      return fail("district has no realizable building region: " + std::to_string(s.district) + reason);
    }
    uint64_t prefix = uint64_t(i + 1) << 32;
    for (auto v : b.blocks)
    {
      v.id |= prefix;
      r.buildings.blocks.push_back(std::move(v));
    }
    for (auto v : b.regions)
    {
      v.id |= prefix;
      v.block |= prefix;
      r.buildings.regions.push_back(std::move(v));
    }
    for (auto v : b.triangles)
    {
      v.region |= prefix;
      r.buildings.triangles.push_back(std::move(v));
    }
    append(b.foundations);
    r.buildings.foundations.insert(r.buildings.foundations.end(), b.foundations.begin(), b.foundations.end());
  }
  if (!arrangeSurfaceContours(contours, in.earthworks.map, terrain, in.planning.revision, r.map, error))
  {
    return false;
  }
  for (auto &b : r.buildings.regions)
  {
    b.frontageEdge = 0;
    for (auto &e : r.map.edges)
    {
      if (e.leftFace && e.rightFace && (street(r.map.faces[e.leftFace - 1].role) != street(r.map.faces[e.rightFace - 1].role)) &&
          distance(b.streetAccess, r.map.vertices[e.a], r.map.vertices[e.b]) <= in.earthworks.map.seamToleranceMetres * 2)
      {
        b.frontageEdge = e.id;
        break;
      }
    }
    if (!b.frontageEdge)
    {
      return fail("building frontage lost during final arrangement");
    }
  }
  if (!meshSurfaceArrangement(r.map, terrain, in.refinement, r.mesh, error))
  {
    return false;
  }
  out = std::move(r);
  return true;
}
} // namespace procedural::world::rewrite
