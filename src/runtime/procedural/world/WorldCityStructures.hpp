#pragma once
#include "WorldCityPlotContracts.hpp"
#include <queue>
#include <tuple>

namespace procedural::world::rewrite {
// World-axis modular assemblies. Bounds are canonical interface coordinates;
// meshing never changes them or adds supports outside the accepted programme.
struct CityStructureBox
{
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
};

struct CityStructureSolid
{
  uint64_t owner = 0;
  CityStructureBox box;
  std::vector<CityStructureBox> openings;
};

struct CityStructureRoute
{
  uint64_t owner = 0;
  std::vector<Position> stations; // level turns are explicit landings
  double width = 1.2, headroom = 2.1;
  bool stairs = false;
};

struct CityStructurePlatform
{
  uint64_t id = 0, programme = 0;
  CityStructureBox box;
  std::vector<uint64_t> additionalProgrammes;
};

struct CityStructureTower
{
  uint64_t id = 0;
  CityStructureBox box; // y1 is the usable top floor, not parapet height
  double walk = 0, thickness = .4;
  // Routes specify real floor holes. The tower's floor is cut only where a
  // stair's headroom intersects it; the landing itself remains a floor.
  std::vector<CityStructureRoute> stairs;
};

struct CityStructureWall
{
  uint64_t id = 0, towerA = 0, towerB = 0;
  Position a, b; // tower endpoints are tower centres; y is the walk level
  double bottom = 0, width = 2, parapet = 1;
  // Portal boxes must pass through this wall and leave a load-bearing lintel.
  std::vector<CityStructureBox> portals;
  bool retaining = false;
};

struct CityStructureBridge
{
  uint64_t id = 0;
  CityStructureBox deck;
  CityStructureBox abutmentA, abutmentB;
  CityStructureBox below;                     // explicit water/neighbourhood clearance
  std::vector<CityStructureRoute> approaches; // both deck ends must be reached
};

struct CityStructureQuay
{
  uint64_t id = 0;
  CityStructureBox bank, water;
};

struct CityStructureProgramme
{
  std::vector<CityStructurePlatform> platforms;
  std::vector<CityStructureTower> towers;
  std::vector<CityStructureWall> walls;
  std::vector<CityStructureRoute> stairs;
  std::vector<CityStructureBridge> bridges;
  std::vector<CityStructureQuay> quays;
  std::vector<CityStructureBox> protectedVolumes;
};

struct CityStructureMesh
{
  std::vector<Position> vertices;
  std::vector<std::array<uint32_t, 3>> triangles;
};

struct CityStructurePlan
{
  uint64_t revision = 0;
  std::shared_ptr<const CityPlotContractPlan> plots;
  std::vector<CityStructureSolid> solids;
  std::set<uint64_t> supportedBuildings;
  std::vector<CityStructureBox> clearance;
  std::vector<CityStructureRoute> routes;
  CityStructureMesh mesh;
  size_t cells = 0;
};

namespace city_structure_detail {
using Box = CityStructureBox;

inline bool valid(Box b)
{
  return std::isfinite(b.x0) && std::isfinite(b.y0) && std::isfinite(b.z0) && std::isfinite(b.x1) && std::isfinite(b.y1) && std::isfinite(b.z1) && b.x1 - b.x0 > 1e-5 && b.y1 - b.y0 > 1e-5 && b.z1 - b.z0 > 1e-5;
}

inline bool contains(Box b, Position p)
{
  return p.x > b.x0 && p.x < b.x1 && p.y > b.y0 && p.y < b.y1 && p.z > b.z0 && p.z < b.z1;
}

inline bool overlap(Box a, Box b)
{
  return std::min(a.x1, b.x1) - std::max(a.x0, b.x0) > 1e-6 && std::min(a.y1, b.y1) - std::max(a.y0, b.y0) > 1e-6 && std::min(a.z1, b.z1) - std::max(a.z0, b.z0) > 1e-6;
}

inline Box routeBox(Position a, Position b, double width, double bottom, double top)
{
  const double h = width * .5;
  if (std::abs(a.x - b.x) > 1e-6)
  {
    return {std::min(a.x, b.x), bottom, a.z - h, std::max(a.x, b.x), top, a.z + h};
  }
  return {a.x - h, bottom, std::min(a.z, b.z), a.x + h, top, std::max(a.z, b.z)};
}

inline std::vector<Position> footprint(Box b)
{
  return {{b.x0, 0, b.z0}, {b.x1, 0, b.z0}, {b.x1, 0, b.z1}, {b.x0, 0, b.z1}};
}

// The arrangement uses only feature coordinates, never a fixed-resolution grid.
// Shared cell faces are removed and all neighbouring faces have identical nodes.
inline bool mesh(CityStructurePlan &plan, const std::function<double(double, double)> &ground, std::string &why)
{
  std::array<std::vector<double>, 3> axes;
  auto add = [&](Box b) {
    axes[0].insert(axes[0].end(), {b.x0, b.x1});
    axes[1].insert(axes[1].end(), {b.y0, b.y1});
    axes[2].insert(axes[2].end(), {b.z0, b.z1});
  };
  for (const auto &s : plan.solids)
  {
    add(s.box);
    for (auto b : s.openings)
    {
      add(b);
    }
  }
  for (auto b : plan.clearance)
  {
    add(b);
  }
  for (auto &a : axes)
  {
    std::sort(a.begin(), a.end());
    a.erase(
        std::unique(
            a.begin(),
            a.end(),
            [](double x, double y) {
              return std::abs(x - y) < 1e-8;
            }),
        a.end());
  }
  if (plan.solids.empty())
  {
    why = "empty structural programme";
    return false;
  }
  size_t nx = axes[0].size() - 1, ny = axes[1].size() - 1, nz = axes[2].size() - 1;
  if (nx > 2000000 / ny || nx * ny > 2000000 / nz)
  {
    why = "structural arrangement budget exceeded";
    return false;
  }
  const auto at = [&](size_t x, size_t y, size_t z) {
    return (x * ny + y) * nz + z;
  };
  if (plan.solids.size() > 4096 || nx * ny * nz > 100000000 / std::max(size_t(1), plan.solids.size()))
  {
    why = "structural classification budget exceeded";
    return false;
  }
  std::vector<uint8_t> occupied(nx * ny * nz), supported(occupied.size());
  std::queue<std::array<size_t, 3>> queue;
  for (size_t x = 0; x < nx; ++x)
  {
    for (size_t y = 0; y < ny; ++y)
    {
      for (size_t z = 0; z < nz; ++z)
      {
        Position p{(axes[0][x] + axes[0][x + 1]) * .5, (axes[1][y] + axes[1][y + 1]) * .5, (axes[2][z] + axes[2][z + 1]) * .5};
        bool solid = false;
        for (const auto &s : plan.solids)
        {
          if (contains(s.box, p))
          {
            bool cut = false;
            for (auto b : s.openings)
            {
              cut |= contains(b, p);
            }
            solid |= !cut;
          }
        }
        if (!solid)
        {
          continue;
        }
        occupied[at(x, y, z)] = 1;
        ++plan.cells;
        for (auto b : plan.clearance)
        {
          if (contains(b, p))
          {
            why = "structure invades reserved passage or headroom";
            return false;
          }
        }
        const double h = ground(p.x, p.z);
        if (!std::isfinite(h))
        {
          why = "nonfinite structural ground";
          return false;
        }
        if (axes[1][y] <= h + 1e-6)
        {
          supported[at(x, y, z)] = 1;
          queue.push({x, y, z});
        }
      }
    }
  }
  // Connected masonry must have real contact with the accepted ground.
  const int directions[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
  while (!queue.empty())
  {
    auto c = queue.front();
    queue.pop();
    for (auto d : directions)
    {
      long x = long(c[0]) + d[0], y = long(c[1]) + d[1], z = long(c[2]) + d[2];
      if (x < 0 || y < 0 || z < 0 || x >= long(nx) || y >= long(ny) || z >= long(nz))
      {
        continue;
      }
      auto i = at(x, y, z);
      if (occupied[i] && !supported[i])
      {
        supported[i] = 1;
        queue.push({size_t(x), size_t(y), size_t(z)});
      }
    }
  }
  for (size_t i = 0; i < occupied.size(); ++i)
  {
    if (occupied[i] && !supported[i])
    {
      why = "unsupported structural component";
      return false;
    }
  }
  // A floor reservation is checked against the actual arrangement, not only
  // against its nominal parent support or a graph-level reachability claim.
  auto solidAt = [&](Position p) {
    for (const auto &s : plan.solids)
    {
      if (p.x >= s.box.x0 - 1e-7 && p.x <= s.box.x1 + 1e-7 && p.y >= s.box.y0 && p.y <= s.box.y1 && p.z >= s.box.z0 - 1e-7 && p.z <= s.box.z1 + 1e-7)
      {
        bool cut = false;
        for (auto b : s.openings)
        {
          cut |= contains(b, p);
        }
        if (!cut)
        {
          return true;
        }
      }
    }
    return false;
  };
  for (auto b : plan.clearance)
  {
    for (double u : {.1, .5, .9})
    {
      for (double v : {.1, .5, .9})
      {
        double x = b.x0 + (b.x1 - b.x0) * u, z = b.z0 + (b.z1 - b.z0) * v;
        double h = ground(x, z);
        if (!std::isfinite(h) || h > b.y0 + 1e-5)
        {
          why = "terrain penetrates reserved clearance";
          return false;
        }
      }
    }
  }
  for (const auto &r : plan.routes)
  {
    for (size_t i = 1; i < r.stations.size(); ++i)
    {
      auto a = r.stations[i - 1], b = r.stations[i];
      double run = std::hypot(b.x - a.x, b.z - a.z);
      unsigned n = std::max(1u, unsigned(std::ceil(run / .2)));
      for (unsigned j = 0; j < n; ++j)
      {
        double t = (j + .5) / n;
        double x = a.x + (b.x - a.x) * t, z = a.z + (b.z - a.z) * t;
        // Stair treads rise to the next station fraction, not a sloped face.
        unsigned steps = std::max(1u, unsigned(std::ceil(std::abs(b.y - a.y) / .18)));
        double y = r.stairs ? a.y + (b.y - a.y) * std::ceil(t * steps) / steps : a.y;
        for (double side : {-.4, 0., .4})
        {
          double px = x - (b.z - a.z) / run * side * r.width, pz = z + (b.x - a.x) / run * side * r.width;
          if (!solidAt({px, y - .01, pz}) && std::abs(ground(px, pz) - y) > .04)
          {
            why = "walking route lacks continuous floor";
            return false;
          }
        }
      }
    }
  }
  std::map<std::tuple<size_t, size_t, size_t>, uint32_t> vertices;
  auto vertex = [&](std::array<size_t, 3> p) {
    auto key = std::make_tuple(p[0], p[1], p[2]);
    auto it = vertices.find(key);
    if (it != vertices.end())
    {
      return it->second;
    }
    uint32_t id = uint32_t(plan.mesh.vertices.size());
    vertices[key] = id;
    plan.mesh.vertices.push_back({axes[0][p[0]], axes[1][p[1]], axes[2][p[2]]});
    return id;
  };
  // Outward winding for x-/x+/y-/y+/z-/z+.
  const unsigned corners[6][4][3] = {
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
    {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}},
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
    {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}},
    {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}},
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}};
  for (size_t x = 0; x < nx; ++x)
  {
    for (size_t y = 0; y < ny; ++y)
    {
      for (size_t z = 0; z < nz; ++z)
      {
        if (occupied[at(x, y, z)])
        {
          for (int side = 0; side < 6; ++side)
          {
            auto d = directions[side];
            long xx = long(x) + d[0], yy = long(y) + d[1], zz = long(z) + d[2];
            if (xx >= 0 && yy >= 0 && zz >= 0 && xx < long(nx) && yy < long(ny) && zz < long(nz) && occupied[at(xx, yy, zz)])
            {
              continue;
            }
            std::array<uint32_t, 4> q;
            for (unsigned k = 0; k < 4; ++k)
            {
              q[k] = vertex({x + corners[side][k][0], y + corners[side][k][1], z + corners[side][k][2]});
            }
            plan.mesh.triangles.push_back({q[0], q[1], q[2]});
            plan.mesh.triangles.push_back({q[0], q[2], q[3]});
          }
        }
      }
    }
  }
  std::map<std::pair<uint32_t, uint32_t>, unsigned> edges;
  for (auto t : plan.mesh.triangles)
  {
    for (unsigned k = 0; k < 3; ++k)
    {
      ++edges[std::minmax(t[k], t[(k + 1) % 3])];
    }
  }
  for (auto [edge, count] : edges)
  {
    if (count != 2)
    {
      why = "nonmanifold structural seam";
      return false;
    }
  }
  return true;
}
} // namespace city_structure_detail

inline bool planCityStructures(std::shared_ptr<const CityPlotContractPlan> plots, const CityStructureProgramme &programme, CityStructurePlan &result, std::string *error = nullptr)
{
  using namespace city_structure_detail;
  const auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!plots || !plots->revision || !plots->terrain || plots->terrain->revision != plots->revision || !plots->terrain->ground)
  {
    return fail("structures require accepted plot and terrain contracts");
  }
  CityStructurePlan p;
  p.revision = plots->revision;
  p.plots = plots;
  std::set<uint64_t> ids;
  auto identity = [&](uint64_t id) {
    return id && ids.insert(id).second;
  };
  const auto add = [&](uint64_t id, Box b, std::vector<Box> holes = std::vector<Box>{}) {
    p.solids.push_back({id, b, std::move(holes)});
  };
  const auto clear = [&](Box b) {
    p.clearance.push_back(b);
  };
  auto stairs = [&](CityStructureRoute r) -> bool {
    if (!r.owner || r.stations.size() < 2 || r.stations.size() > 64 || !std::isfinite(r.width) || r.width < .9 || !std::isfinite(r.headroom) || r.headroom < 2.1)
    {
      return false;
    }
    r.stairs = true;
    for (size_t i = 1; i < r.stations.size(); ++i)
    {
      auto a = r.stations[i - 1], b = r.stations[i];
      double dx = std::abs(b.x - a.x), dz = std::abs(b.z - a.z), rise = b.y - a.y;
      if (!std::isfinite(dx + dz + rise) || std::min(dx, dz) > 1e-6 || dx + dz < .1 || rise < 0 || rise > 12)
      {
        return false;
      }
      unsigned n = std::max(1u, unsigned(std::ceil(rise / .18)));
      if ((dx + dz) / n < .3)
      {
        return false;
      }
      for (unsigned j = 0; j < n; ++j)
      {
        auto lo = city_terrain_detail::lerp(a, b, double(j) / n), hi = city_terrain_detail::lerp(a, b, double(j + 1) / n);
        double bottom = std::min(a.y - .3, plots->terrain->ground(lo.x, lo.z) - .1);
        add(r.owner, routeBox(lo, hi, r.width, bottom, hi.y));
        clear(routeBox(lo, hi, r.width, hi.y, hi.y + r.headroom));
      }
    }
    // A turn cannot be made at a zero-area corner. Adjacent flights meet
    // through a caller-planned level landing at least one full module long.
    for (size_t i = 1; i + 1 < r.stations.size(); ++i)
    {
      auto a = r.stations[i - 1], b = r.stations[i], c = r.stations[i + 1];
      bool turn = (std::abs(a.x - b.x) < 1e-6) != (std::abs(b.x - c.x) < 1e-6);
      if (turn)
      {
        if (std::abs(a.y - b.y) > 1e-6 || std::abs(b.y - c.y) > 1e-6)
        {
          return false;
        }
        double h = r.width * .5;
        add(r.owner, {b.x - h, std::min(b.y - .3, plots->terrain->ground(b.x, b.z) - .1), b.z - h, b.x + h, b.y, b.z + h});
        clear({b.x - h, b.y, b.z - h, b.x + h, b.y + r.headroom, b.z + h});
      }
    }
    p.routes.push_back(r);
    return true;
  };
  for (auto s : programme.platforms)
  {
    if (!identity(s.id) || !s.programme || !valid(s.box))
    {
      return fail("invalid or unused platform");
    }
    auto programmes = s.additionalProgrammes;
    programmes.push_back(s.programme);
    for (auto building : programmes)
    {
      if (!p.supportedBuildings.insert(building).second)
      {
        return fail("duplicate platform support ownership");
      }
      bool used = false;
      for (const auto &lot : plots->plots)
      {
        if (lot.building == building)
        {
          used = std::abs(lot.floor - s.box.y1) < 1e-6;
          for (auto v : lot.envelope)
          {
            used &= v.x >= s.box.x0 && v.x <= s.box.x1 && v.z >= s.box.z0 && v.z <= s.box.z1;
          }
        }
      }
      if (!used)
      {
        return fail("platform does not support its contracted dwelling");
      }
    }
    add(s.id, s.box);
  }
  for (const auto &t : programme.towers)
  {
    if (!identity(t.id) || !valid(t.box) || !std::isfinite(t.walk + t.thickness) || t.thickness < .2 || t.walk < t.box.y0 + .3 || t.box.y1 < t.walk + 2.4 || t.box.x1 - t.box.x0 < 2 * t.thickness + 2 ||
        t.box.z1 - t.box.z0 < 2 * t.thickness + 2)
    {
      return fail("invalid tower floor programme");
    }
    for (auto r : t.stairs)
    {
      if (r.owner != t.id || !stairs(r))
      {
        return fail("invalid tower stair or landing");
      }
      for (auto a : r.stations)
      {
        if (a.x - r.width * .5 < t.box.x0 + t.thickness || a.x + r.width * .5 > t.box.x1 - t.thickness || a.z - r.width * .5 < t.box.z0 + t.thickness || a.z + r.width * .5 > t.box.z1 - t.thickness)
        {
          return fail("tower stair exceeds chamber");
        }
      }
    }
    if (t.stairs.empty())
    {
      return fail("tower top has no planned stair access");
    }
    bool reached = false;
    for (auto r : t.stairs)
    {
      reached |= std::abs(r.stations.front().y - t.walk) < 1e-6 && std::abs(r.stations.back().y - t.box.y1) < 1e-6;
    }
    if (!reached)
    {
      return fail("tower stair does not connect walk to top floor");
    }
    std::vector<Box> holes;
    for (auto b : p.clearance)
    {
      if (overlap(b, t.box))
      {
        holes.push_back(b);
      }
    }
    add(t.id, {t.box.x0, t.walk - .25, t.box.z0, t.box.x1, t.walk, t.box.z1});
    add(t.id, {t.box.x0, t.box.y1 - .25, t.box.z0, t.box.x1, t.box.y1, t.box.z1}, holes);
    std::vector<Box> ports;
    for (auto w : programme.walls)
    {
      if (w.towerA == t.id || w.towerB == t.id)
      {
        auto a = w.towerA == t.id ? w.a : w.b;
        auto b = w.towerA == t.id ? w.b : w.a;
        if (std::abs(a.y - t.walk) > 1e-6 || std::abs(a.x - (t.box.x0 + t.box.x1) * .5) > 1e-6 || std::abs(a.z - (t.box.z0 + t.box.z1) * .5) > 1e-6)
        {
          return fail("wall socket and tower walk disagree");
        }
        auto port = routeBox(a, b, w.width, t.walk, t.walk + 2.1);
        ports.push_back(port);
        clear(port);
        p.routes.push_back({t.id, {a, b}, w.width, 2.1, false});
      }
    }
    // Walls and parapets are one shell. Portals are removed only from the
    // shell, never from the floor below the doorway.
    auto b = t.box;
    double h = t.thickness;
    add(t.id, {b.x0, b.y0, b.z0, b.x0 + h, b.y1 + 1, b.z1}, ports);
    add(t.id, {b.x1 - h, b.y0, b.z0, b.x1, b.y1 + 1, b.z1}, ports);
    add(t.id, {b.x0 + h, b.y0, b.z0, b.x1 - h, b.y1 + 1, b.z0 + h}, ports);
    add(t.id, {b.x0 + h, b.y0, b.z1 - h, b.x1 - h, b.y1 + 1, b.z1}, ports);
  }
  for (auto w : programme.walls)
  {
    if (!identity(w.id) || !std::isfinite(w.width + w.bottom + w.parapet) || w.width < .9 || w.parapet < 0 || w.parapet > 1.5 || std::abs(w.a.y - w.b.y) > 1e-6)
    {
      return fail("wall requires level walk and valid dimensions");
    }
    auto a = w.a, b = w.b;
    if (std::min(std::abs(a.x - b.x), std::abs(a.z - b.z)) > 1e-6)
    {
      return fail("wall must follow modular axis");
    }
    auto trim = [&](uint64_t id, Position &v, Position other) {
      if (!id)
      {
        return true;
      }
      auto t = std::find_if(programme.towers.begin(), programme.towers.end(), [&](const auto &t) {
        return t.id == id;
      });
      if (t == programme.towers.end())
      {
        return false;
      }
      if (std::abs(v.x - other.x) > 1e-6)
      {
        v.x = other.x > v.x ? t->box.x1 : t->box.x0;
      }
      else
      {
        v.z = other.z > v.z ? t->box.z1 : t->box.z0;
      }
      return true;
    };
    if (!trim(w.towerA, a, b) || !trim(w.towerB, b, a))
    {
      return fail("missing tower socket");
    }
    auto wall = routeBox(a, b, w.width + (w.retaining ? 0 : .4), w.bottom, a.y);
    if (!valid(wall))
    {
      return fail("empty wall or support taller than walk");
    }
    for (size_t i = 0; i < w.portals.size(); ++i)
    {
      auto portal = w.portals[i];
      bool alongX = std::abs(a.x - b.x) > 1e-6;
      bool through = alongX ? (portal.z0 <= wall.z0 && portal.z1 >= wall.z1 && portal.x0 >= wall.x0 + .3 && portal.x1 <= wall.x1 - .3 && portal.x1 - portal.x0 >= .9)
                            : (portal.x0 <= wall.x0 && portal.x1 >= wall.x1 && portal.z0 >= wall.z0 + .3 && portal.z1 <= wall.z1 - .3 && portal.z1 - portal.z0 >= .9);
      if (!valid(portal) || !through || portal.y1 - portal.y0 < 2.1 || portal.y1 > wall.y1 - .3 || portal.y0 < wall.y0 || !overlap(portal, wall))
      {
        return fail("invalid gate lintel or opening");
      }
      for (size_t j = 0; j < i; ++j)
      {
        if (overlap(portal, w.portals[j]))
        {
          return fail("overlapping gate apertures");
        }
      }
      clear(portal);
    }
    add(w.id, wall, w.portals);
    if (!w.retaining)
    {
      clear(routeBox(a, b, w.width, a.y, a.y + 2.1));
      p.routes.push_back({w.id, {a, b}, w.width, 2.1, false});
    }
    if (w.parapet > 0 && !w.retaining)
    {
      auto left = wall, right = wall;
      left.y0 = right.y0 = a.y;
      left.y1 = right.y1 = a.y + w.parapet;
      if (std::abs(a.x - b.x) > 1e-6)
      {
        left.z1 = left.z0 + .2;
        right.z0 = right.z1 - .2;
      }
      else
      {
        left.x1 = left.x0 + .2;
        right.x0 = right.x1 - .2;
      }
      add(w.id, left);
      add(w.id, right);
    }
  }
  for (auto r : programme.stairs)
  {
    if (!identity(r.owner) || !stairs(r))
    {
      return fail("invalid stair flight or turning landing");
    }
  }
  for (auto b : programme.bridges)
  {
    if (!identity(b.id) || !valid(b.deck) || !valid(b.abutmentA) || !valid(b.abutmentB) || !valid(b.below) || b.below.y1 > b.deck.y0 ||
        !overlap({b.deck.x0, b.deck.y0 - 1e-4, b.deck.z0, b.deck.x1, b.deck.y1, b.deck.z1}, b.abutmentA) || !overlap({b.deck.x0, b.deck.y0 - 1e-4, b.deck.z0, b.deck.x1, b.deck.y1, b.deck.z1}, b.abutmentB) ||
        std::abs(b.abutmentA.y1 - b.deck.y0) > 1e-6 || std::abs(b.abutmentB.y1 - b.deck.y0) > 1e-6)
    {
      return fail("bridge lacks matched abutments or clearance");
    }
    bool alongX = b.deck.x1 - b.deck.x0 >= b.deck.z1 - b.deck.z0;
    const double centreX = (b.deck.x0 + b.deck.x1) * .5, centreZ = (b.deck.z0 + b.deck.z1) * .5;
    Position a = alongX ? Position{b.deck.x0, b.deck.y1, centreZ} : Position{centreX, b.deck.y1, b.deck.z0};
    Position end = alongX ? Position{b.deck.x1, b.deck.y1, centreZ} : Position{centreX, b.deck.y1, b.deck.z1};
    bool reachedA = false, reachedB = false;
    for (auto r : b.approaches)
    {
      if (r.owner != b.id || !stairs(r))
      {
        return fail("invalid bridge approach");
      }
      auto first = r.stations.front(), last = r.stations.back();
      if (std::abs(first.y - plots->terrain->ground(first.x, first.z)) > .04)
      {
        return fail("bridge approach has no ground arrival");
      }
      auto near = [](Position p, Position q) {
        return std::hypot(p.x - q.x, p.z - q.z) < 1e-5 && std::abs(p.y - q.y) < 1e-5;
      };
      reachedA |= near(last, a);
      reachedB |= near(last, end);
    }
    if (!reachedA || !reachedB)
    {
      return fail("bridge requires access at both ends");
    }
    bool bearings = alongX ? (b.abutmentA.x0 == b.deck.x0 && b.abutmentB.x1 == b.deck.x1 && b.abutmentA.z0 <= b.deck.z0 && b.abutmentA.z1 >= b.deck.z1 && b.abutmentB.z0 <= b.deck.z0 && b.abutmentB.z1 >= b.deck.z1)
                           : (b.abutmentA.z0 == b.deck.z0 && b.abutmentB.z1 == b.deck.z1 && b.abutmentA.x0 <= b.deck.x0 && b.abutmentA.x1 >= b.deck.x1 && b.abutmentB.x0 <= b.deck.x0 && b.abutmentB.x1 >= b.deck.x1);
    if (!bearings)
    {
      return fail("bridge bearings must support opposite full-width ends");
    }
    p.routes.push_back({b.id, {a, end}, alongX ? b.deck.z1 - b.deck.z0 : b.deck.x1 - b.deck.x0, 2.1, false});
    add(b.id, b.deck);
    add(b.id, b.abutmentA);
    add(b.id, b.abutmentB);
    clear(b.below);
    clear({b.deck.x0, b.deck.y1, b.deck.z0, b.deck.x1, b.deck.y1 + 2.1, b.deck.z1});
  }
  for (auto q : programme.quays)
  {
    if (!identity(q.id) || !valid(q.bank) || !valid(q.water) || overlap(q.bank, q.water) || q.bank.y1 < q.water.y1 + .3)
    {
      return fail("quay lacks dry freeboard or invades channel");
    }
    const bool touchX = (std::abs(q.bank.x1 - q.water.x0) < 1e-6 || std::abs(q.bank.x0 - q.water.x1) < 1e-6) && std::min(q.bank.z1, q.water.z1) > std::max(q.bank.z0, q.water.z0);
    const bool touchZ = (std::abs(q.bank.z1 - q.water.z0) < 1e-6 || std::abs(q.bank.z0 - q.water.z1) < 1e-6) && std::min(q.bank.x1, q.water.x1) > std::max(q.bank.x0, q.water.x0);
    if (!touchX && !touchZ)
    {
      return fail("quay must share a bank seam with its channel");
    }
    add(q.id, q.bank);
    clear(q.water);
  }
  // Low side guards share the flight's support. Clearances carve guards,
  // never supports: landing exits cannot be fenced shut by late decoration.
  const auto walkingClearance = p.clearance;
  for (const auto &r : p.routes)
  {
    if (r.stairs)
    {
      for (size_t i = 1; i < r.stations.size(); ++i)
      {
        auto a = r.stations[i - 1], b = r.stations[i];
        unsigned n = std::max(1u, unsigned(std::ceil((b.y - a.y) / .18)));
        for (unsigned j = 0; j < n; ++j)
        {
          auto lo = city_terrain_detail::lerp(a, b, double(j) / n), hi = city_terrain_detail::lerp(a, b, double(j + 1) / n);
          auto box = routeBox(lo, hi, r.width + .3, std::min(a.y - .3, plots->terrain->ground(lo.x, lo.z) - .1), hi.y + .9);
          auto left = box, right = box;
          if (std::abs(a.x - b.x) > 1e-6)
          {
            left.z1 = left.z0 + .15;
            right.z0 = right.z1 - .15;
          }
          else
          {
            left.x1 = left.x0 + .15;
            right.x0 = right.x1 - .15;
          }
          add(r.owner, left, walkingClearance);
          add(r.owner, right, walkingClearance);
        }
      }
    }
  }
  for (auto b : programme.protectedVolumes)
  {
    if (!valid(b))
    {
      return fail("invalid protected volume");
    }
    clear(b);
  }
  for (const auto &s : p.solids)
  {
    if (!valid(s.box))
    {
      return fail("invalid structural solid");
    }
    for (auto b : s.openings)
    {
      if (!valid(b))
      {
        return fail("invalid structural opening");
      }
    }
    for (auto &lot : plots->plots)
    {
      if (s.box.y1 > lot.floor + 1e-6 && city_plot_detail::overlap(footprint(s.box), lot.lot))
      {
        return fail("structure invades contracted dwelling");
      }
      if (s.box.y1 > lot.sidewalk + 1e-6 && city_plot_detail::overlap(footprint(s.box), lot.approach))
      {
        return fail("structure invades contracted doorway approach");
      }
    }
  }
  // Circulation is a graph separate from structural support. Ground contact
  // alone cannot make an elevated wall walk or tower floor reachable.
  std::vector<bool> reached(p.routes.size(), false);
  const auto sameLevel = [](Position a, Position b) {
    return std::abs(a.y - b.y) < 1e-5;
  };
  const auto connected = [&](Position a, Position b) {
    if (!sameLevel(a, b))
    {
      return false;
    }
    if (std::hypot(a.x - b.x, a.z - b.z) < 1e-5)
    {
      return true;
    }
    return false;
  };
  for (size_t i = 0; i < p.routes.size(); ++i)
  {
    for (auto q : {p.routes[i].stations.front(), p.routes[i].stations.back()})
    {
      reached[i] = reached[i] || std::abs(q.y - plots->terrain->ground(q.x, q.z)) < .04;
    }
  }
  bool changed = true;
  while (changed)
  {
    changed = false;
    for (size_t i = 0; i < p.routes.size(); ++i)
    {
      if (reached[i])
      {
        for (size_t j = 0; j < p.routes.size(); ++j)
        {
          if (!reached[j])
          {
            for (auto a : {p.routes[i].stations.front(), p.routes[i].stations.back()})
            {
              for (auto b : {p.routes[j].stations.front(), p.routes[j].stations.back()})
              {
                if (connected(a, b))
                {
                  reached[j] = true;
                  changed = true;
                }
              }
            }
          }
        }
      }
    }
  }
  for (bool r : reached)
  {
    if (!r)
    {
      return fail("elevated structure lacks connected ground access");
    }
  }
  std::string why;
  if (!mesh(p, plots->terrain->ground, why))
  {
    return fail(why);
  }
  result = std::move(p);
  if (error)
  {
    error->clear();
  }
  return true;
}
} // namespace procedural::world::rewrite
