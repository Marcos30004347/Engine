#include "DwellingPlanner.hpp"

#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace procedural::world::generator {

namespace {

using rewrite::DwellingBrief;
using rewrite::DwellingDoor;
using rewrite::DwellingFloor;
using rewrite::DwellingPlan;
using rewrite::DwellingRoom;
using rewrite::DwellingStair;
using rewrite::DwellingUse;
using rewrite::DwellingWall;
using rewrite::Position;
using rewrite::RoomUse;

// Axis-aligned rectangle in the wing's local frame (x along the entrance edge, z inward).
struct Rect
{
  double x0 = 0, z0 = 0, x1 = 0, z1 = 0;

  double width() const
  {
    return x1 - x0;
  }

  double depth() const
  {
    return z1 - z0;
  }

  double area() const
  {
    return width() * depth();
  }
};

struct Cell
{
  Rect rect;
  RoomUse use = RoomUse::Living;
  bool hall = false; // Circulation, not a room.
};

// Length of the shared boundary between two rectangles, and where it lies.
struct Shared
{
  bool vertical = false; // Shared edge runs along z.
  double at = 0;         // The edge coordinate (x for vertical, z for horizontal).
  double lo = 0, hi = 0; // Overlap interval along the edge.
};

bool sharedEdge(const Rect &a, const Rect &b, double tolerance, Shared &out)
{
  if (std::abs(a.x1 - b.x0) < tolerance || std::abs(b.x1 - a.x0) < tolerance)
  {
    out.vertical = true;
    out.at = std::abs(a.x1 - b.x0) < tolerance ? a.x1 : a.x0;
    out.lo = std::max(a.z0, b.z0);
    out.hi = std::min(a.z1, b.z1);
    return out.hi - out.lo > tolerance;
  }

  if (std::abs(a.z1 - b.z0) < tolerance || std::abs(b.z1 - a.z0) < tolerance)
  {
    out.vertical = false;
    out.at = std::abs(a.z1 - b.z0) < tolerance ? a.z1 : a.z0;
    out.lo = std::max(a.x0, b.x0);
    out.hi = std::min(a.x1, b.x1);
    return out.hi - out.lo > tolerance;
  }

  return false;
}

double jitter(uint64_t seed, uint64_t id, uint64_t purpose, uint64_t index)
{
  return double(rewrite::featureSeed(seed, 5, id, purpose, index) % 100000) / 100000.;
}

// Bounded recursive guillotine cuts. A cut is taken along the longer axis at a
// seeded position that leaves both sides at least the minimum width.
void subdivide(const Rect &rect, double targetArea, double minimumWidth, int depth, uint64_t seed, uint64_t id, uint64_t &counter, std::vector<Rect> &out)
{
  // A child must keep a usable width and at least 60% of the target area, so
  // no cut produces a sliver that the wall allowance would swallow.
  const double minimumChildArea = targetArea * .6;
  const bool canSplitX = rect.width() >= 2 * minimumWidth && rect.depth() * minimumWidth >= minimumChildArea;
  const bool canSplitZ = rect.depth() >= 2 * minimumWidth && rect.width() * minimumWidth >= minimumChildArea;

  if (depth >= 6 || rect.area() <= targetArea * 1.6 || (!canSplitX && !canSplitZ))
  {
    out.push_back(rect);
    return;
  }

  const bool alongX = canSplitX && (!canSplitZ || rect.width() >= rect.depth());
  const double length = alongX ? rect.width() : rect.depth();
  const double other = alongX ? rect.depth() : rect.width();
  const double minimumCut = std::max(minimumWidth, minimumChildArea / other);
  const double span = length - 2 * minimumCut;

  if (span <= 0)
  {
    out.push_back(rect);
    return;
  }

  const double t = .5 + (jitter(seed, id, 3, counter++) - .5) * .5;
  const double cut = minimumCut + span * t;

  Rect first = rect, second = rect;

  if (alongX)
  {
    first.x1 = rect.x0 + cut;
    second.x0 = rect.x0 + cut;
  }
  else
  {
    first.z1 = rect.z0 + cut;
    second.z0 = rect.z0 + cut;
  }

  subdivide(first, targetArea, minimumWidth, depth + 1, seed, id, counter, out);
  subdivide(second, targetArea, minimumWidth, depth + 1, seed, id, counter, out);
}

} // namespace

bool planDwelling(const DwellingBrief &b, DwellingPlan &out, std::string *error)
{
  auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  if (!b.id || b.shell.size() != 4 || b.floors < 1 || b.floors > 3)
  {
    return fail("planner supports rectangular wings with one to three floors");
  }

  if (b.floorRise < 2.4 || b.floorRise > 3.4 || b.wallThickness < .12 || b.wallThickness > .5 || b.availableHeight + 1e-6 < b.floors * b.floorRise)
  {
    return fail("insufficient shell height or invalid structure");
  }

  if (b.use == DwellingUse::ShopHouse && b.floors < 2)
  {
    return fail("shop-house requires a separate domestic upper floor");
  }

  // Local frame: origin at the entrance edge's first vertex, x along it.
  size_t edge = 4;
  double nearest = 1e30, entry = 0;

  for (size_t i = 0; i < 4; ++i)
  {
    auto a = b.shell[i], c = b.shell[(i + 1) % 4];
    double l = std::hypot(c.x - a.x, c.z - a.z);

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

  const auto origin = b.shell[edge], end = b.shell[(edge + 1) % 4];
  const double W = std::hypot(end.x - origin.x, end.z - origin.z), ux = (end.x - origin.x) / W, uz = (end.z - origin.z) / W;

  auto local = [&](Position p) {
    return Position{(p.x - origin.x) * ux + (p.z - origin.z) * uz, 0, -(p.x - origin.x) * uz + (p.z - origin.z) * ux};
  };

  const double D = local(b.shell[(edge + 2) % 4]).z;

  if (W < 5 || D < 5 || W > 80 || D > 80)
  {
    return fail("planner needs a wing of at least 5 by 5 metres");
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

  auto ring = [&](const Rect &r, double y) {
    return std::vector<Position>{point(r.x0, r.z0, y), point(r.x1, r.z0, y), point(r.x1, r.z1, y), point(r.x0, r.z1, y)};
  };

  const double t = b.wallThickness;
  const double minimumWidth = 2.4;

  // Jettying: cumulative street-facing overhang per floor, bounded per floor
  // (0.6 m) and in total (1.0 m); the ground floor stays on its footprint.
  std::vector<double> overhang(b.floors, 0);

  for (uint32_t f = 1; f < b.floors; ++f)
  {
    const double step = f < b.jettyMetres.size() ? b.jettyMetres[f] : 0;

    if (!std::isfinite(step) || step < 0 || step > .6)
    {
      return fail("jetty projection per floor must lie within 0 and 0.6 m");
    }

    overhang[f] = overhang[f - 1] + step;

    if (overhang[f] > 1.0 + 1e-9)
    {
      return fail("cumulative jetty projection exceeds 1.0 m");
    }
  }

  // Entrance hall: a short hall inside the door; as wide as the stair core
  // when there is one, so the side rooms share a wall with it.
  const bool stairs = b.floors >= 2;
  const double flightWidth = 1.0;
  const double hallHalf = stairs ? flightWidth + .05 : .8;

  if (entry - hallHalf < t + .2 || entry + hallHalf > W - t - .2)
  {
    return fail("fixed entrance sits too close to a corner for a hall");
  }

  Rect hall{entry - hallHalf, t, entry + hallHalf, std::min(D - t, 3.2)};

  // Dog-leg stair core behind the hall: two flights of 1.0 m with a half
  // landing. Total risers from the floor rise; each flight takes half.
  const uint32_t risers = uint32_t(std::ceil(b.floorRise / .18));
  const uint32_t risersA = risers / 2, risersB = risers - risersA;
  const double tread = .27, landing = 1.1;
  const double run = std::max(risersA, risersB) > 1 ? (std::max(risersA, risersB) - 1) * tread : tread;
  Rect core{entry - flightWidth - .05, hall.z1, entry + flightWidth + .05, hall.z1 + run + landing};

  if (stairs && (core.z1 + minimumWidth > D - t || core.x0 < t || core.x1 > W - t))
  {
    return fail("wing cannot hold the entrance hall, the turning stair and rooms behind");
  }

  if (stairs && core.x0 - t < minimumWidth && W - t - core.x1 < minimumWidth)
  {
    return fail("wing too narrow for rooms beside the turning stair");
  }

  // Free blocks on each floor: two front blocks beside the hall/core and a
  // full-width back band behind them. Back rooms are reached through the
  // side rooms when a stair core separates them from the hall.
  auto blocks = [&](bool withCore, double front) {
    std::vector<Rect> result;
    const double reservedX0 = withCore ? std::min(hall.x0, core.x0) : hall.x0, reservedX1 = withCore ? std::max(hall.x1, core.x1) : hall.x1;
    double reservedZ1 = withCore ? core.z1 : hall.z1;
    const bool backBand = D - t - reservedZ1 >= minimumWidth;

    if (!backBand)
    {
      reservedZ1 = D - t; // Side blocks run the full depth.
    }

    if (reservedX0 - t >= minimumWidth)
    {
      result.push_back({t, front + t, reservedX0, reservedZ1});
    }

    if (W - t - reservedX1 >= minimumWidth)
    {
      result.push_back({reservedX1, front + t, W - t, reservedZ1});
    }

    if (backBand)
    {
      result.push_back({t, reservedZ1, W - t, D - t});
    }

    return result;
  };

  double targetArea = 14;

  switch (b.use)
  {
  case DwellingUse::ShopHouse:
    targetArea = 18;
    break;
  case DwellingUse::Lodging:
    targetArea = 16;
    break;
  case DwellingUse::Hall:
  case DwellingUse::Warehouse:
    targetArea = 400;
    break;
  default:
    break;
  }

  DwellingPlan p;
  p.id = b.id;
  p.seed = b.seed;
  p.use = b.use;

  uint64_t wallId = 1, roomId = 1, counter = 0;

  auto addWall = [&](Position a, Position z, uint32_t floor, bool exterior) {
    for (auto &w : p.walls)
    {
      if (w.floor == floor &&
          ((std::hypot(w.a.x - a.x, w.a.z - a.z) < 1e-6 && std::hypot(w.b.x - z.x, w.b.z - z.z) < 1e-6) || (std::hypot(w.a.x - z.x, w.a.z - z.z) < 1e-6 && std::hypot(w.b.x - a.x, w.b.z - a.z) < 1e-6)))
      {
        w.exterior |= exterior;

        return w.id;
      }
    }

    auto id = wallId++;
    p.walls.push_back({id, floor, a, z, t, exterior});

    return id;
  };

  for (uint32_t f = 0; f < b.floors; ++f)
  {
    const double y = b.foundation + f * b.floorRise;
    const bool coreHere = stairs && f + 1 < b.floors; // A flight leaves this floor.
    const bool arrival = stairs && f > 0;             // A flight arrives here.

    DwellingFloor floor;
    floor.elevation = y;
    floor.height = b.floorRise;
    floor.grossArea = W * D;

    std::vector<Cell> cells;

    Cell hallCell;
    hallCell.rect = hall;
    hallCell.hall = true;
    cells.push_back(hallCell);

    if (coreHere || arrival)
    {
      floor.coreArea = core.area();
    }

    const double front = -overhang[f]; // Jettied floors start ahead of the ground footprint.

    for (const auto &block : blocks(coreHere || arrival, front))
    {
      std::vector<Rect> rooms;
      subdivide(block, targetArea, minimumWidth, 0, b.seed, b.id + f * 7919, counter, rooms);

      for (const auto &room : rooms)
      {
        Cell cell;
        cell.rect = room;
        cells.push_back(cell);
      }
    }

    if (cells.size() < 2)
    {
      return fail("floor has no room beside the hall");
    }

    // Connectivity: doors from the hall outward, breadth first over shared walls.
    std::vector<int> connected(cells.size(), -1);
    connected[0] = 0;
    std::vector<size_t> frontier{0};
    std::vector<std::pair<size_t, size_t>> doors; // (room, through)

    while (!frontier.empty())
    {
      std::vector<size_t> next;

      for (auto from : frontier)
      {
        for (size_t to = 1; to < cells.size(); ++to)
        {
          Shared shared;

          if (connected[to] >= 0 || !sharedEdge(cells[from].rect, cells[to].rect, 1e-6, shared) || shared.hi - shared.lo < 1.2)
          {
            continue;
          }

          connected[to] = int(from);
          doors.push_back({to, from});
          next.push_back(to);
        }
      }

      frontier = std::move(next);
    }

    for (size_t k = 1; k < cells.size(); ++k)
    {
      if (connected[k] < 0)
      {
        return fail(
            "a room cannot be reached from the hall through a legal door (" + std::to_string(cells[k].rect.width()) + " x " + std::to_string(cells[k].rect.depth()) + " at z " + std::to_string(cells[k].rect.z0) +
            " in a " + std::to_string(W) + " x " + std::to_string(D) + " wing)");
      }
    }

    // Purposes: the largest hall-adjacent room is the main room of the programme.
    std::vector<size_t> order;

    for (size_t k = 1; k < cells.size(); ++k)
    {
      order.push_back(k);
    }

    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t c) {
      const bool hallA = connected[a] == 0, hallC = connected[c] == 0;

      if (hallA != hallC)
      {
        return hallA;
      }

      return cells[a].rect.area() > cells[c].rect.area();
    });

    for (size_t n = 0; n < order.size(); ++n)
    {
      auto &cell = cells[order[n]];
      const bool front = cell.rect.z0 < D * .5;

      switch (b.use)
      {
      case DwellingUse::Cottage:
        cell.use = f == 0 ? (n == 0 ? RoomUse::Living : n == 1 && order.size() > 2 ? RoomUse::Service : RoomUse::Sleeping) : (n == 0 && order.size() > 2 ? RoomUse::Living : RoomUse::Sleeping);
        break;
      case DwellingUse::ShopHouse:
        cell.use = f == 0 ? (front || n == 0 ? RoomUse::Shop : RoomUse::Store) : (n == 0 ? RoomUse::Living : n == 1 && order.size() > 2 ? RoomUse::Service : RoomUse::Sleeping);
        break;
      case DwellingUse::Lodging:
        cell.use = f == 0 ? (n == 0 ? RoomUse::Common : n == 1 ? RoomUse::Service : RoomUse::Sleeping) : RoomUse::Sleeping;
        break;
      case DwellingUse::Hall:
        cell.use = n == 0 ? RoomUse::Hall : RoomUse::Service;
        break;
      default:
        cell.use = RoomUse::Store;
        break;
      }
    }

    // Publish rooms, walls and doors for this floor.
    std::vector<uint64_t> roomIds(cells.size(), 0);

    for (size_t k = 1; k < cells.size(); ++k)
    {
      const auto &cell = cells[k];
      Rect inner{cell.rect.x0 + t * .5, cell.rect.z0 + t * .5, cell.rect.x1 - t * .5, cell.rect.z1 - t * .5};

      if (inner.width() < 2 || inner.depth() < 2 || inner.area() < 6)
      {
        return fail(
            "room falls below usable size after walls (" + std::to_string(inner.width()) + " x " + std::to_string(inner.depth()) + " m in a " + std::to_string(W) + " x " + std::to_string(D) + " wing, entry " +
            std::to_string(entry) + ")");
      }

      DwellingRoom room;
      room.id = roomId++;
      room.floor = f;
      room.use = cell.use;
      room.boundary = ring(inner, y);
      room.usableArea = inner.area();
      floor.roomArea += room.usableArea;

      if (room.use == RoomUse::Sleeping)
      {
        p.residentCapacity += uint32_t(room.usableArea / (b.use == DwellingUse::Lodging ? 4. : 6.));
      }

      if (room.use == RoomUse::Shop || room.use == RoomUse::Hall || room.use == RoomUse::Store)
      {
        p.workerCapacity += uint32_t(room.usableArea / 8.);
      }

      roomIds[k] = room.id;
      p.rooms.push_back(room);
    }

    for (size_t k = 0; k < cells.size(); ++k)
    {
      const auto gross = ring(cells[k].rect, y);

      for (size_t e = 0; e < 4; ++e)
      {
        const auto &r = cells[k].rect;
        const bool exterior = (e == 0 && r.z0 <= front + t + 1e-6) || (e == 1 && r.x1 >= W - t - 1e-6) || (e == 2 && r.z1 >= D - t - 1e-6) || (e == 3 && r.x0 <= t + 1e-6);
        addWall(gross[e], gross[(e + 1) % 4], f, exterior);
      }
    }

    {
      const Rect plate{0, front, W, D};
      const auto outer = ring(plate, y);

      for (size_t e = 0; e < 4; ++e)
      {
        addWall(outer[e], outer[(e + 1) % 4], f, true);
      }
    }

    for (auto [room, through] : doors)
    {
      Shared shared;
      sharedEdge(cells[room].rect, cells[through].rect, 1e-6, shared);
      const double mid = (shared.lo + shared.hi) * .5;
      const Position a = shared.vertical ? point(shared.at, shared.lo, y) : point(shared.lo, shared.at, y);
      const Position c = shared.vertical ? point(shared.at, shared.hi, y) : point(shared.hi, shared.at, y);
      uint64_t wall = 0;

      for (const auto &w : p.walls)
      {
        if (w.floor != f)
        {
          continue;
        }

        // The door's wall is the canonical wall segment containing the shared interval.
        const double len = std::hypot(w.b.x - w.a.x, w.b.z - w.a.z);
        const double ta = ((a.x - w.a.x) * (w.b.x - w.a.x) + (a.z - w.a.z) * (w.b.z - w.a.z)) / (len * len), tc = ((c.x - w.a.x) * (w.b.x - w.a.x) + (c.z - w.a.z) * (w.b.z - w.a.z)) / (len * len);
        const double da = std::abs((w.b.x - w.a.x) * (a.z - w.a.z) - (w.b.z - w.a.z) * (a.x - w.a.x)) / len, dc = std::abs((w.b.x - w.a.x) * (c.z - w.a.z) - (w.b.z - w.a.z) * (c.x - w.a.x)) / len;

        if (da < 1e-5 && dc < 1e-5 && ta > -1e-6 && tc > -1e-6 && ta < 1 + 1e-6 && tc < 1 + 1e-6)
        {
          wall = w.id;
          break;
        }
      }

      if (!wall)
      {
        return fail("door interval lies on no canonical wall");
      }

      const bool privateDoor = cells[room].use == RoomUse::Sleeping || f > 0;
      p.doors.push_back({wall, roomIds[room], f, shared.vertical ? point(shared.at, mid, y) : point(mid, shared.at, y), .9, privateDoor, false});
    }

    // Shop-house selling opening: a broad opening on the front exterior wall
    // of the ground-floor shop room, separate from the private hall door.
    if (f == 0 && b.use == DwellingUse::ShopHouse)
    {
      for (size_t k = 1; k < cells.size(); ++k)
      {
        const auto &cell = cells[k];

        if (cell.use != RoomUse::Shop || cell.rect.z0 > t + 1e-6 || cell.rect.width() < 2.8)
        {
          continue;
        }

        const double openingWidth = std::min(2.6, cell.rect.width() - 1.0);
        const double centre = (cell.rect.x0 + cell.rect.x1) * .5;
        uint64_t frontWall = 0;

        for (const auto &w : p.walls)
        {
          if (w.floor == 0 && std::abs(local(w.a).z) < 1e-6 && std::abs(local(w.b).z) < 1e-6 && std::min(local(w.a).x, local(w.b).x) <= centre - openingWidth * .5 + 1e-6 &&
              std::max(local(w.a).x, local(w.b).x) >= centre + openingWidth * .5 - 1e-6)
          {
            frontWall = w.id;
            break;
          }
        }

        if (frontWall)
        {
          DwellingDoor opening{frontWall, roomIds[k], 0, point(centre, 0, y), openingWidth, false, true};
          opening.shopOpening = true;
          p.doors.push_back(opening);
        }
      }
    }

    if (f == 0)
    {
      uint64_t frontWall = 0;

      for (const auto &w : p.walls)
      {
        if (w.floor == 0 && std::abs(local(w.a).z) < 1e-6 && std::abs(local(w.b).z) < 1e-6 && std::min(local(w.a).x, local(w.b).x) <= entry + 1e-6 && std::max(local(w.a).x, local(w.b).x) >= entry - 1e-6)
        {
          frontWall = w.id;
          break;
        }
      }

      if (!frontWall)
      {
        return fail("entrance wall not found");
      }

      auto door = b.entrance;
      door.y = y;
      p.doors.push_back({frontWall, 0, 0, door, .95, false, true});
    }

    // Circulation and slabs.
    floor.circulation.push_back(ring(hall, y));
    floor.circulationArea += hall.area();

    if (coreHere)
    {
      // Flight A climbs away from the hall on the left, the landing turns, flight B returns on the right.
      Rect flightA{core.x0, core.z0, entry - .05, core.z0 + run};
      Rect landingRect{core.x0, core.z0 + run, core.x1, core.z1};
      Rect flightB{entry + .05, core.z0, core.x1, core.z0 + run};

      DwellingStair a;
      a.opening = ring(flightA, y + b.floorRise);
      a.lowerLanding = ring(Rect{flightA.x0, hall.z0, flightA.x1, hall.z1}, y);
      a.upperLanding = ring(landingRect, y + risersA * (b.floorRise / risers));
      a.lowerFloor = f;
      a.risers = risersA;
      a.rise = risersA * (b.floorRise / risers);
      a.width = flightWidth;
      a.privateAccess = b.use == DwellingUse::ShopHouse || b.use == DwellingUse::Cottage;
      p.stairs.push_back(a);

      DwellingStair second;
      second.opening = ring(flightB, y + b.floorRise);
      second.lowerLanding = ring(landingRect, y + risersA * (b.floorRise / risers));
      second.upperLanding = ring(Rect{flightB.x0, hall.z0, flightB.x1, hall.z1}, y + b.floorRise);
      second.lowerFloor = f;
      second.risers = risersB;
      second.rise = risersB * (b.floorRise / risers);
      second.startRise = a.rise;
      second.width = flightWidth;
      second.privateAccess = a.privateAccess;
      p.stairs.push_back(second);

      floor.circulation.push_back(ring(landingRect, y));
      floor.circulationArea += landingRect.area();
    }

    if (arrival)
    {
      floor.slabs = {ring(Rect{0, front, W, core.z0}, y), ring(Rect{0, core.z1, W, D}, y), ring(Rect{0, core.z0, core.x0, core.z1}, y), ring(Rect{core.x1, core.z0, W, core.z1}, y)};
    }
    else
    {
      floor.slabs = {ring(Rect{0, front, W, D}, y)};
    }

    floor.grossArea = W * (D - front);
    floor.wallArea = std::max(0., floor.grossArea - floor.roomArea - floor.circulationArea - floor.coreArea);
    p.floors.push_back(std::move(floor));
  }

  if ((b.use == DwellingUse::Cottage || b.use == DwellingUse::ShopHouse || b.use == DwellingUse::Lodging) && !p.residentCapacity)
  {
    return fail("residential programme has no usable beds");
  }

  if (error != nullptr)
  {
    error->clear();
  }

  out = std::move(p);

  return true;
}

} // namespace procedural::world::generator
