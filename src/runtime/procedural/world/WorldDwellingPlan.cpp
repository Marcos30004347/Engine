#include "WorldDwellingPlan.hpp"
#include "MapPartition.hpp"
#include "generator/DwellingPlanner.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
using Poly = std::vector<Position>;

double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return std::abs(a) * .5;
}

double distance(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
} // namespace

const char *furnitureKindName(const FurnitureKind kind)
{
  switch (kind)
  {
  case FurnitureKind::Bed:
    return "bed";
  case FurnitureKind::Table:
    return "table";
  case FurnitureKind::Counter:
    return "counter";
  case FurnitureKind::Hearth:
    return "hearth";
  case FurnitureKind::Chest:
    return "chest";
  case FurnitureKind::Bench:
    return "bench";
  case FurnitureKind::Workbench:
    return "workbench";
  }

  return "?";
}

bool generateDwellingPlan(const DwellingBrief &b, DwellingPlan &out, std::string *error)
{
  auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  // The planner (hall, turning stair, recursive room cuts, door graph) first;
  // the legacy two-band recipe remains a validated fallback and says so.
  std::string plannerError;
  if (b.id && b.shell.size() == 4 && b.floors >= 1 && b.floors <= 3)
  {
    DwellingPlan planned;
    if (generator::planDwelling(b, planned, &plannerError))
    {
      out = std::move(planned);
      if (error)
      {
        error->clear();
      }
      return true;
    }
    if (b.floors > 2)
    {
      return fail("dwelling planner: " + plannerError);
    }
  }
  if (!b.id || b.shell.size() != 4 || b.floors < 1 || b.floors > 2 || unsigned(b.use) > unsigned(DwellingUse::Warehouse))
  {
    return fail("unsupported dwelling identity, shell or floors");
  }
  for (double v : {b.foundation, b.floorRise, b.availableHeight, b.wallThickness, b.entrance.x, b.entrance.z})
  {
    if (!std::isfinite(v))
    {
      return fail("nonfinite dwelling brief");
    }
  }
  if (b.floorRise < 2.4 || b.floorRise > 3.4 || b.wallThickness < .12 || b.wallThickness > .5 || b.availableHeight + 1e-6 < b.floors * b.floorRise)
  {
    return fail("insufficient shell height or invalid structure");
  }
  if (b.use == DwellingUse::ShopHouse && b.floors != 2)
  {
    return fail("shop-house requires separate domestic upper floor");
  }
  size_t edge = 4;
  double nearest = 1e30, entry = 0;
  for (size_t i = 0; i < 4; ++i)
  {
    auto a = b.shell[i], c = b.shell[(i + 1) % 4];
    for (double v : {a.x, a.y, a.z})
    {
      if (!std::isfinite(v) || std::abs(v) > 1e6)
      {
        return fail("invalid shell coordinate");
      }
    }
    double l = distance(a, c);
    if (l < 1)
    {
      return fail("degenerate shell");
    }
    double t = ((b.entrance.x - a.x) * (c.x - a.x) + (b.entrance.z - a.z) * (c.z - a.z)) / (l * l);
    double d = std::hypot(b.entrance.x - a.x - t * (c.x - a.x), b.entrance.z - a.z - t * (c.z - a.z));
    if (t >= 0 && t <= 1 && d < nearest)
    {
      nearest = d;
      edge = i;
      entry = t * l;
    }
  }
  if (edge == 4 || nearest > 1e-5)
  {
    return fail("fixed entrance is not on shell");
  }
  auto origin = b.shell[edge], end = b.shell[(edge + 1) % 4];
  double W = distance(origin, end), ux = (end.x - origin.x) / W, uz = (end.z - origin.z) / W;
  auto local = [&](Position p) {
    return Position{(p.x - origin.x) * ux + (p.z - origin.z) * uz, 0, -(p.x - origin.x) * uz + (p.z - origin.z) * ux};
  };
  double D = local(b.shell[(edge + 2) % 4]).z;
  if (W < 2.8 || D < 2.8 || W > 80 || D > 80)
  {
    return fail("unsupported wing dimensions");
  }
  for (size_t k = 0; k < 4; ++k)
  {
    auto p = local(b.shell[(edge + k) % 4]);
    double x = (k == 1 || k == 2) ? W : 0, z = k >= 2 ? D : 0;
    if (std::hypot(p.x - x, p.z - z) > 1e-5)
    {
      return fail("shell is not a CCW rectangle");
    }
  }
  auto point = [&](double x, double z, double y) {
    return Position{origin.x + ux * x - uz * z, y, origin.z + uz * x + ux * z};
  };
  auto box = [&](double x0, double z0, double x1, double z1, double y) {
    return Poly{point(x0, z0, y), point(x1, z0, y), point(x1, z1, y), point(x0, z1, y)};
  };
  // Compact cottages use a connected living room and chamber directly, rather
  // than sacrificing a narrow shell to two room bands plus a corridor.
  if (b.use == DwellingUse::Cottage && b.floors == 1 && (W < 6.4 || D < 5.6 || entry < 3.1 || W - entry < 3.1))
  {
    const double t = b.wallThickness;
    if (W * D > 180 || entry < t + .5 || entry > W - t - .5)
    {
      return fail("compact cottage fixed entrance lacks clearance");
    }
    bool across = D >= 5.2;
    double cut = across ? D * .5 : W * .5;
    if (!across && std::abs(entry - cut) < .8)
    {
      cut = entry < W * .5 ? entry + .8 : entry - .8;
    }
    std::vector<procedural::Point2> sites = across ? std::vector<procedural::Point2>{{W * .5, cut * .5}, {W * .5, (cut + D) * .5}} : std::vector<procedural::Point2>{{cut * .5, D * .5}, {(cut + W) * .5, D * .5}};
    // Position sites symmetrically around the selected shared partition.
    if (across)
    {
      sites[0].y = cut - 1;
      sites[1].y = cut + 1;
    }
    else
    {
      sites[0].x = cut - 1;
      sites[1].x = cut + 1;
    }
    std::vector<procedural::world::VoronoiCell> cells;
    if (!procedural::world::buildVoronoiCells(sites, {{t, t}, {W - t, t}, {W - t, D - t}, {t, D - t}}, 1e-6, cells, error))
    {
      return false;
    }
    DwellingPlan p;
    p.id = b.id;
    p.seed = b.seed;
    p.use = b.use;
    for (size_t k = 0; k < 4; ++k)
    {
      p.walls.push_back({k + 1, 0, point(k == 1 || k == 2 ? W : 0, k >= 2 ? D : 0, b.foundation), point((k + 1) % 4 == 1 || (k + 1) % 4 == 2 ? W : 0, (k + 1) % 4 >= 2 ? D : 0, b.foundation), t, true});
    }
    p.walls.push_back({5, 0, across ? point(t, cut, b.foundation) : point(cut, t, b.foundation), across ? point(W - t, cut, b.foundation) : point(cut, D - t, b.foundation), t, false});
    uint32_t living = across ? 0 : entry < cut ? 0 : 1;
    DwellingFloor floor;
    floor.elevation = b.foundation;
    floor.height = b.floorRise;
    floor.grossArea = W * D;
    floor.slabs = {box(0, 0, W, D, b.foundation)};
    for (size_t k = 0; k < cells.size(); ++k)
    {
      double x0 = 1e30, z0 = 1e30, x1 = -1e30, z1 = -1e30;
      for (auto v : cells[k].area)
      {
        x0 = std::min(x0, v.x);
        x1 = std::max(x1, v.x);
        z0 = std::min(z0, v.y);
        z1 = std::max(z1, v.y);
      }
      if (across)
      {
        if (k == 0)
        {
          z1 -= t * .5;
        }
        else
        {
          z0 += t * .5;
        }
      }
      else
      {
        if (k == 0)
        {
          x1 -= t * .5;
        }
        else
        {
          x0 += t * .5;
        }
      }
      if (x1 - x0 < 2.2 || z1 - z0 < 2.2)
      {
        return fail("compact room lacks useful width");
      }
      DwellingRoom room;
      room.id = k + 1;
      room.use = k == living ? RoomUse::Living : RoomUse::Sleeping;
      room.boundary = box(x0, z0, x1, z1, b.foundation);
      room.usableArea = area(room.boundary);
      if (room.usableArea < 6)
      {
        return fail("compact room lacks usable area");
      }
      floor.roomArea += room.usableArea;
      if (room.use == RoomUse::Sleeping)
      {
        p.residentCapacity = uint32_t(room.usableArea / 6);
      }
      p.rooms.push_back(room);
    }
    auto door = b.entrance;
    door.y = b.foundation;
    p.doors.push_back({1, living + 1, 0, door, .9, false, true});
    p.doors.push_back({5, 2 - living, 0, across ? point(W * .5, cut, b.foundation) : point(cut, D * .5, b.foundation), .9, true, false});
    floor.wallArea = floor.grossArea - floor.roomArea;
    p.floors.push_back(floor);
    if (!p.residentCapacity)
    {
      return fail("compact cottage has no sleeping capacity");
    }
    p.fallback = true;
    p.note = plannerError;
    if (error)
    {
      error->clear();
    }
    out = std::move(p);
    return true;
  }
  const double t = b.wallThickness, corridor = 1.4, cl = entry - corridor * .5, cr = entry + corridor * .5;
  if (cl - t < 2.2 || W - t - cr < 2.2)
  {
    return fail("fixed entrance cannot serve two usable room bands");
  }
  uint32_t risers = uint32_t(std::ceil(b.floorRise / .18));
  double run = (risers - 1) * .27, landing = 1.2, coreLength = run + 2 * landing;
  double roomEnd = b.floors == 2 ? D - t - coreLength - t * .5 : D - t;
  const double rearStart = b.floors == 2 ? roomEnd + t * .5 : roomEnd;
  if (roomEnd - t < (b.floors == 2 ? 3.5 : 2.6))
  {
    return fail("wing cannot retain rooms beside reserved stair assembly");
  }
  DwellingPlan p;
  p.id = b.id;
  p.seed = b.seed;
  p.use = b.use;
  uint64_t wallId = 1, roomId = 1;
  auto addWall = [&](Position a, Position z, uint32_t floor) {
    for (auto &w : p.walls)
    {
      if (w.floor == floor && ((distance(w.a, a) < 1e-6 && distance(w.b, z) < 1e-6) || (distance(w.a, z) < 1e-6 && distance(w.b, a) < 1e-6)))
      {
        return w.id;
      }
    }
    auto id = wallId++;
    p.walls.push_back({id, floor, a, z, t});
    return id;
  };
  for (uint32_t f = 0; f < b.floors; ++f)
  {
    double y = b.foundation + f * b.floorRise;
    DwellingFloor floor;
    floor.elevation = y;
    floor.height = b.floorRise;
    floor.grossArea = W * D;
    floor.circulation.push_back(box(cl + t * .5, t, cr - t * .5, rearStart, y));
    double flightStart = rearStart + landing, flightEnd = flightStart + run, sx = entry - .55, ex = entry + .55;
    if (b.floors == 2)
    {
      floor.circulation.push_back(box(t, rearStart, sx, D - t, y));
      floor.circulation.push_back(box(ex, rearStart, W - t, D - t, y));
      floor.circulation.push_back(box(sx, rearStart, ex, flightStart, y));
      floor.circulation.push_back(box(sx, flightEnd, ex, D - t, y));
      floor.coreArea = 1.1 * run;
      if (f == 0)
      {
        p.stairs.push_back({box(sx, flightStart, ex, flightEnd, y + b.floorRise), box(sx, rearStart, ex, flightStart, y), box(sx, flightEnd, ex, D - t, y + b.floorRise), 0, risers, b.floorRise, .27, 1.1});
      }
    }
    if (!p.stairs.empty())
    {
      p.stairs.back().privateAccess = b.use == DwellingUse::ShopHouse || b.use == DwellingUse::Cottage;
    }
    for (auto &q : floor.circulation)
    {
      floor.circulationArea += area(q);
    }
    if (f == 1)
    {
      floor.slabs = {box(0, 0, W, flightStart, y), box(0, flightEnd, W, D, y), box(0, flightStart, sx, flightEnd, y), box(ex, flightStart, W, flightEnd, y)};
    }
    else
    {
      floor.slabs = {box(0, 0, W, D, y)};
    }
    for (size_t k = 0; k < 4; ++k)
    {
      addWall(point(k == 1 || k == 2 ? W : 0, k >= 2 ? D : 0, y), point((k + 1) % 4 == 1 || (k + 1) % 4 == 2 ? W : 0, (k + 1) % 4 >= 2 ? D : 0, y), f);
    }
    for (auto &wall : p.walls)
    {
      if (wall.floor == f)
      {
        wall.exterior = true;
      }
    }
    if (f == 0)
    {
      auto id = addWall(point(0, 0, y), point(W, 0, y), f);
      p.doors.push_back({id, 0, f, b.entrance, .95, false, true});
      p.doors.back().center.y = y;
    }
    for (int side = 0; side < 2; ++side)
    {
      double x0 = side ? cr : t, x1 = side ? W - t : cl;
      uint32_t count = b.use == DwellingUse::Lodging ? 3 : (b.use == DwellingUse::Hall || b.use == DwellingUse::Warehouse || b.use == DwellingUse::ShopHouse && f == 0 ? 1 : 2);
      if (b.use == DwellingUse::Cottage && b.floors == 1 && roomEnd - t < 5.2)
      {
        count = 1; // Combined living/service room in compact cottages.
      }
      if ((roomEnd - t) / count < 2.4)
      {
        return fail("mandatory room programme lacks depth");
      }
      std::vector<procedural::Point2> sites;
      for (uint32_t k = 0; k < count; ++k)
      {
        double jitter = (double(featureSeed(b.seed, 1, b.id, side + f * 2, k) % 1000) / 1000. - .5) * .15;
        sites.push_back({(x0 + x1) * .5, t + (roomEnd - t) * (k + .5 + jitter) / count});
      }
      std::vector<procedural::world::VoronoiCell> cells;
      if (!procedural::world::buildVoronoiCells(sites, {{x0, t}, {x1, t}, {x1, roomEnd}, {x0, roomEnd}}, 1e-6, cells, error))
      {
        return false;
      }
      for (size_t k = 0; k < cells.size(); ++k)
      {
        double z0 = 1e30, z1 = -1e30;
        for (auto v : cells[k].area)
        {
          z0 = std::min(z0, v.y);
          z1 = std::max(z1, v.y);
        }
        if (z1 - z0 - t < 2.2 || x1 - x0 - t < 2)
        {
          return fail("Voronoi room is below usable width");
        }
        RoomUse use = RoomUse::Sleeping;
        if (b.use == DwellingUse::Cottage)
        {
          use = side ? (k ? RoomUse::Service : RoomUse::Living) : RoomUse::Sleeping;
        }
        if (b.use == DwellingUse::ShopHouse)
        {
          use = f == 0 ? (side ? RoomUse::Store : RoomUse::Shop) : (side ? (k ? RoomUse::Service : RoomUse::Living) : RoomUse::Sleeping);
        }
        if (b.use == DwellingUse::Lodging && side && k < 2)
        {
          use = k ? RoomUse::Service : RoomUse::Common;
        }
        if (b.use == DwellingUse::Hall)
        {
          use = side ? RoomUse::Service : RoomUse::Hall;
        }
        if (b.use == DwellingUse::Warehouse)
        {
          use = RoomUse::Store;
        }
        DwellingRoom room;
        room.id = roomId++;
        room.floor = f;
        room.use = use;
        room.boundary = box(x0 + t * .5, z0 + t * .5, x1 - t * .5, z1 - t * .5, y);
        room.usableArea = area(room.boundary);
        if (room.usableArea < 6)
        {
          return fail("mandatory room area is infeasible");
        }
        floor.roomArea += room.usableArea;
        if (use == RoomUse::Sleeping)
        {
          p.residentCapacity += uint32_t(room.usableArea / (b.use == DwellingUse::Lodging ? 4. : 6.));
        }
        if (use == RoomUse::Shop || use == RoomUse::Hall || use == RoomUse::Store)
        {
          p.workerCapacity += uint32_t(room.usableArea / 8.);
        }
        double doorX = side ? x0 : x1, doorZ = (z0 + z1) * .5;
        auto gross = box(x0, z0, x1, z1, y);
        uint64_t doorWall = 0;
        for (size_t e = 0; e < 4; ++e)
        {
          auto id = addWall(gross[e], gross[(e + 1) % 4], f);
          auto a = local(gross[e]), c = local(gross[(e + 1) % 4]);
          if (std::abs(a.x - doorX) < 1e-5 && std::abs(c.x - doorX) < 1e-5)
          {
            doorWall = id;
          }
        }
        if (!doorWall)
        {
          return fail("room has no canonical corridor wall");
        }
        p.doors.push_back({doorWall, room.id, f, point(doorX, doorZ, y), .9, use == RoomUse::Sleeping || f > 0, false});
        p.rooms.push_back(room);
      }
    }
    floor.wallArea = floor.grossArea - floor.roomArea - floor.circulationArea - floor.coreArea;
    if (floor.wallArea < 0)
    {
      return fail("invalid dwelling area ledger");
    }
    p.floors.push_back(std::move(floor));
  }
  if ((b.use == DwellingUse::Cottage || b.use == DwellingUse::ShopHouse || b.use == DwellingUse::Lodging) && !p.residentCapacity)
  {
    return fail("residential programme has no usable beds");
  }
  p.fallback = true;
  p.note = plannerError;
  if (error)
  {
    error->clear();
  }
  out = std::move(p);
  return true;
}

bool meshDwellingPlan(const DwellingPlan &p, std::vector<DwellingTriangle> &out, std::string *error)
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!p.id || p.floors.empty() || p.floors.size() > 3)
  {
    return fail("invalid dwelling mesh input");
  }
  std::vector<DwellingTriangle> mesh;
  auto prism = [&](const Poly &ring, double bottom, double top, uint32_t material) {
    for (size_t k = 0; k < ring.size(); ++k)
    {
      auto a = ring[k], b = ring[(k + 1) % ring.size()], c = b, d = a;
      a.y = b.y = bottom;
      c.y = d.y = top;
      mesh.push_back({a, b, c, material});
      mesh.push_back({a, c, d, material});
    }
    for (size_t k = 1; k + 1 < ring.size(); ++k)
    {
      auto a = ring[0], b = ring[k], c = ring[k + 1];
      a.y = b.y = c.y = top;
      mesh.push_back({a, c, b, material});
      a.y = b.y = c.y = bottom;
      mesh.push_back({a, b, c, material});
    }
  };
  for (auto &f : p.floors)
  {
    for (auto &slab : f.slabs)
    {
      prism(slab, f.elevation - .18, f.elevation, 0);
    }
  }
  for (auto &w : p.walls)
  {
    if (w.floor >= p.floors.size() || distance(w.a, w.b) < 1e-6 || w.thickness <= 0)
    {
      return fail("invalid wall");
    }
    double length = distance(w.a, w.b), dx = (w.b.x - w.a.x) / length, dz = (w.b.z - w.a.z) / length;
    double H = p.floors[w.floor].height;
    auto segment = [&](double lo, double hi, double bottom, double top) {
      if (hi - lo < 1e-6)
      {
        return;
      }
      Poly q;
      for (auto v : std::vector<std::pair<double, double>>{
             {lo, w.exterior ? 0 : -w.thickness * .5}, {hi, w.exterior ? 0 : -w.thickness * .5}, {hi, w.exterior ? w.thickness : w.thickness * .5}, {lo, w.exterior ? w.thickness : w.thickness * .5}})
      {
        q.push_back({w.a.x + dx * v.first - dz * v.second, 0, w.a.z + dz * v.first + dx * v.second});
      }
      prism(q, bottom, top, 1);
    };
    std::vector<std::pair<double, double>> openings;
    for (auto &d : p.doors)
    {
      if (d.wall == w.id)
      {
        double t = (d.center.x - w.a.x) * dx + (d.center.z - w.a.z) * dz;
        double lo = t - d.width * .5, hi = t + d.width * .5;
        if (lo < 0 || hi > length || d.floor != w.floor)
        {
          return fail("door leaves wall");
        }
        openings.push_back({lo, hi});
      }
    }
    std::sort(openings.begin(), openings.end());
    double at = 0;
    for (auto [lo, hi] : openings)
    {
      if (lo < at)
      {
        return fail("overlapping doors");
      }
      segment(at, lo, w.a.y, w.a.y + H - .18);
      segment(lo, hi, w.a.y + 2.05, w.a.y + H - .18);
      at = hi;
    }
    segment(at, length, w.a.y, w.a.y + H - .18);
  }
  for (auto &s : p.stairs)
  {
    if (s.opening.size() != 4 || s.risers < 2 || s.rise <= 0 || s.lowerFloor + 1 >= p.floors.size())
    {
      return fail("invalid stair");
    }
    auto a = s.opening[0], b = s.opening[1], c = s.opening[3];
    double low = p.floors[s.lowerFloor].elevation + s.startRise;
    for (uint32_t k = 0; k < s.risers - 1; ++k)
    {
      Poly q;
      for (auto [u, v] : std::vector<std::pair<double, double>>{{0., double(k) / (s.risers - 1)}, {1., double(k) / (s.risers - 1)}, {1., double(k + 1) / (s.risers - 1)}, {0., double(k + 1) / (s.risers - 1)}})
      {
        q.push_back({a.x + (b.x - a.x) * u + (c.x - a.x) * v, 0, a.z + (b.z - a.z) * u + (c.z - a.z) * v});
      }
      prism(q, low, low + (k + 1) * s.rise / s.risers, 2);
    }
  }
  for (auto &t : mesh)
  {
    for (auto v : {t.a, t.b, t.c})
    {
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
      {
        return fail("nonfinite dwelling mesh");
      }
    }
  }
  if (error)
  {
    error->clear();
  }
  out = std::move(mesh);
  return true;
}
} // namespace procedural::world::rewrite
