#include "DwellingPlanner.hpp"

#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <functional>
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

  // Stop at a *drawn* size rather than at a fixed multiple of the target.
  //
  // A single threshold makes every room the same size, which is what the
  // gallery render showed: a grid of near-identical boxes. Watabou sizes a lot
  // as target * 2^(k(r - 0.5)), a log-uniform spread about the target, and the
  // mix of large and small is most of what makes a plan read as a building
  // rather than as a table. The draw is addressed by the cut counter so it does
  // not depend on recursion order.
  const double spread = std::pow(2., 1.1 * (jitter(seed, id, 11, counter) - .5));

  if (depth >= 6 || rect.area() <= targetArea * 1.6 * spread || (!canSplitX && !canSplitZ))
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

  // A single-storey shop-house is a shop with the family living behind it,
  // which is a building that exists; declining it cost 49 of the 368 declines
  // measured over the lot corpus and built nothing in their place. The
  // programme below already puts the shop at the front and living behind when
  // there is no upper floor, so the rule was asserting a requirement the
  // planner did not actually have.

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
  double hallHalf = stairs ? flightWidth + .05 : .8;

  if (entry - hallHalf < t + .2 || entry + hallHalf > W - t - .2)
  {
    return fail("fixed entrance sits too close to a corner for a hall");
  }

  // The hall is a share of the plate, not a fixed 3.2 m.
  //
  // A constant hall spends two thirds of a five metre plate on circulation
  // before a single room is cut, and what is left cannot hold a stair. A small
  // house has a short hall; a large one can afford a long one.
  // Floored at a usable room depth, because the blocks *beside* the hall are
  // only as deep as the hall is. Scaling it down to 1.4 m on a small plate
  // turned those side blocks into 1.6 m slivers that the usable-size rule then
  // refused, which is how a castle's stable wing and a manor's service wing
  // stopped being planned at all.
  double hallDepth = std::clamp(D * .30, minimumWidth + t, 3.2);

  // A plate too small for a hall does without one.
  //
  // The entrance opens straight into the room and the stair stands in a
  // corner of it - which is what a one-room cottage is, and the most common
  // building any settlement has. Insisting on a hall refused every two storey
  // plate of about six metres: 50 of 735 over the lot corpus, all of them for
  // the same reason, and all of them shapes that plainly exist.
  //
  // The hall is not removed so much as reduced to the doorway it always was on
  // a plate this size, so nothing downstream needs a second code path: the
  // entrance still lands on it, the circulation is still counted, and the rest
  // of the plate is one room.
  const double wantsBehind = (b.floors >= 2 ? std::max(1.9, flightWidth * 1.9) : 0.0) + minimumWidth;
  const bool roomForAHall = t + hallDepth + wantsBehind <= D - t;

  if (!roomForAHall)
  {
    // A threshold across the full width, not a short hall down the middle.
    //
    // Narrowing the hall leaves blocks beside it only as deep as it is, and on
    // a five metre plate that is a two metre sliver the usable-size rule then
    // refuses - 30 of the 40 declines that remained. Spanning the width means
    // there are no side blocks at all: the plate is a threshold and one room
    // behind it, which is the shape of every small cottage ever built.
    hallDepth = std::max(.9, std::min(hallDepth, D - t - wantsBehind - t));
    hallHalf = std::min(hallHalf, .55);
  }

  // A deep plate gets a corridor, not a hall.
  //
  // D2: a corridor is a room with a purpose, not the absence of one. With a
  // short hall at the front, anything behind the first rank of rooms is reached
  // *through* them - which is how a plan ends up with a bedroom you walk
  // through to reach another bedroom. Where there is depth for two ranks, the
  // hall runs the whole way instead and the rooms hang off it, so every room
  // has its own door onto the circulation.
  //
  // The test is two rooms' depth behind the hall: less than that and a single
  // back band is reached from the hall directly, which needs no spine.
  // Depth to need a corridor, and width to afford one. Testing only the depth
  // put a 2.1 m spine down the middle of a 5 m plate and left 1.45 m either
  // side, which is not a room - 160 declines of 735 where there had been 10.
  // D2, the corridor, is PARKED and not merely disabled.
  //
  // It works and the panels show it: on a deep, wide plate the hall runs the
  // whole depth and every room opens onto it instead of through its neighbour.
  // What it breaks is elsewhere - the settlement director retains tenancies
  // across a growth transaction and requires a re-planned house to still hold
  // the household living in it, and a corridor changes a plan's capacity
  // enough to break that.
  //
  // Three attempts to keep both failed, and each failed for a reason worth
  // recording: making the decision independent of the floor count, then of the
  // hall depth that the floor count reaches through, then restricting it to
  // plates of twelve by nine metres. The director still lost four checks, so
  // the cause is not the flip I assumed it was. The invariant is the director's
  // to state - a re-plan that shrinks a house should re-house its tenants, not
  // fail the transaction - and until it does, a corridor cannot be turned on.
  const bool wantsSpine = false;
  const double spineHalf = flightWidth + .05;

  if (wantsSpine)
  {
    // Wide enough to carry the stair, since the core sits in it.
    hallHalf = std::max(hallHalf, spineHalf);
  }

  Rect hall{std::max(t, entry - hallHalf), t, std::min(W - t, entry + hallHalf), wantsSpine ? D - t : std::min(D - t, t + hallDepth)};

  // The stair core, by type: the first kind that fits the plate behind the
  // hall wins.
  //
  // One hard-coded dog-leg was the single largest cause of a refused plan. It
  // needs 2.1 m of width and about 3.0 m of depth *behind* a 3.2 m hall, so any
  // two-storey wing shallower than roughly 8.8 m was refused outright - 290 of
  // 368 declines over the lot corpus, 79 per cent of them, on two rules that
  // were both really the same rule. Watabou's Dwellings names `spiral` and
  // `stairwell` as the two forms a house uses, and a spiral is exactly the
  // answer to a plate with no room for a turning flight: it is one room's
  // footprint and climbs a whole storey inside it.
  const uint32_t risers = uint32_t(std::ceil(b.floorRise / .18));
  const double tread = .27, landing = 1.1;
  const uint32_t risersA = risers / 2, risersB = risers - risersA;
  const double run = std::max(risersA, risersB) > 1 ? (std::max(risersA, risersB) - 1) * tread : tread;

  // A spiral wide enough to be walkable: two tread lengths across the newel.
  const double spiralSide = std::max(1.9, flightWidth * 1.9);

  bool spiral = false;
  bool stairInRoom = false;

  // Behind the hall normally; at the far end of the corridor when there is one,
  // because a core placed "behind" a full depth spine would be outside the
  // plate, and one placed beside it would block a side band.
  Rect core = wantsSpine ? Rect{hall.x0, std::max(t, D - t - (run + landing)), hall.x1, D - t} : Rect{entry - flightWidth - .05, hall.z1, entry + flightWidth + .05, hall.z1 + run + landing};

  const auto coreFits = [&](const Rect &candidate) {
    // Inside the shell first: a core that pokes through a wall is not a core.
    if (candidate.x0 < t || candidate.x1 > W - t || candidate.z1 > D - t)
    {
      return false;
    }

    // In a corridor the core stands at the far end of it, and the rooms are
    // beside the corridor rather than behind the core: there is nothing to ask
    // about ground behind it.
    if (wantsSpine)
    {
      return candidate.z0 > hall.z0 + minimumWidth && (candidate.x0 - t >= minimumWidth || W - t - candidate.x1 >= minimumWidth);
    }

    // Then somewhere for a room to go, measured on the *union* of the hall and
    // the core - which is what the block layout actually reserves.
    //
    // Measuring the core alone accepted a stair tucked against one wall while
    // the hall stood in the middle: the core left 2.7 m clear on its right, the
    // union left 1.75 m, and the floor came out with no room on it at all.
    // Every remaining decline over the lot corpus was that disagreement.
    const double behind = D - t - std::max(candidate.z1, hall.z1);
    const double left = std::min(candidate.x0, hall.x0) - t;
    const double right = W - t - std::max(candidate.x1, hall.x1);

    return behind >= minimumWidth || left >= minimumWidth || right >= minimumWidth;
  };

  if (stairs && !coreFits(core))
  {
    const Rect spiralCore = wantsSpine ? Rect{hall.x0, std::max(t, D - t - spiralSide), hall.x1, D - t} : Rect{entry - spiralSide * .5, hall.z1, entry + spiralSide * .5, hall.z1 + spiralSide};

    if (coreFits(spiralCore))
    {
      core = spiralCore;
      spiral = true;
    }
    else
    {
      // Last resort before declining: a spiral tucked against the side wall
      // with the hall running past it, which is what a cottage does when the
      // plate has no depth to spare at all.
      const bool leftRoom = W - t - (t + spiralSide) >= minimumWidth;
      const Rect aside = leftRoom ? Rect{t, hall.z1, t + spiralSide, hall.z1 + spiralSide} : Rect{W - t - spiralSide, hall.z1, W - t, hall.z1 + spiralSide};

      if (coreFits(aside))
      {
        core = aside;
        spiral = true;
      }
      else
      {
        // Nowhere to reserve a core, so the stair stands in a corner of the
        // room instead of in a space of its own.
        //
        // That is what a tower is, and what the smallest cottages are: the
        // flight is furniture, not a room. Reserving one was the last thing
        // refusing plates outright - the remaining one per cent of the lot
        // corpus, and every compound tower, which at four and a half metres
        // across cannot give a stair its own footprint and still hold a room.
        stairInRoom = true;
        core = Rect{W - t - spiralSide, D - t - spiralSide, W - t, D - t};
        spiral = true;
      }
    }
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

    // A side block is only offered if it is deep enough to be a room, not just
    // wide enough. Beside a short hall on a shallow plate it is two metres
    // deep, which the usable-size rule then refuses - and the refusal fails
    // the whole plan rather than the block, so 30 of 40 remaining declines
    // were this one shape. Not offering it leaves the plate as a threshold and
    // a room behind, which is what it is.
    const bool deepEnough = reservedZ1 - (front + t) >= minimumWidth;

    if (deepEnough && reservedX0 - t >= minimumWidth)
    {
      result.push_back({t, front + t, reservedX0, reservedZ1});
    }

    if (deepEnough && W - t - reservedX1 >= minimumWidth)
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

    // The stair core is circulation too, and is a cell for the same reason the
    // hall is: a door is placed on the wall shared by two *cells*, so a room
    // that touches only the core had nowhere to put its door. Marking rooms
    // reachable without recording that door left them with no access door at
    // all, which the director's own ledger caught - every room must have
    // exactly one.
    const bool coreCellHere = (coreHere || arrival) && !stairInRoom;

    if (coreCellHere)
    {
      Cell coreCell;
      coreCell.rect = core;
      coreCell.hall = true;
      cells.push_back(coreCell);
    }

    if ((coreHere || arrival) && !stairInRoom)
    {
      floor.coreArea = core.area();
    }

    const double front = -overhang[f]; // Jettied floors start ahead of the ground footprint.

    for (const auto &block : blocks((coreHere || arrival) && !stairInRoom, front))
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

    // cells[0] is the hall, so this is "no room besides the circulation".
    //
    // Relaxing it to allow a floor of hall alone produced forty plans that
    // declared a storey with no room on it, which the gallery's own invariant
    // caught. The plates that fail here are six metres and under with two
    // floors, and they fail honestly: a hall, a stair and a room of the
    // minimum width do not fit. The answer is a cottage with no hall - the
    // door opening straight into the room, the stair in its corner - which is
    // a shape this planner cannot yet draw, not a check to loosen.
    size_t roomCells = 0;

    for (const Cell &cell : cells)
    {
      roomCells += cell.hall ? 0u : 1u;
    }

    if (roomCells == 0u)
    {
      return fail("floor has no room beside the hall (" + std::to_string(W) + " x " + std::to_string(D) + " wing, floor " + std::to_string(f) + " of " + std::to_string(b.floors) + ")");
    }

    // Connectivity: doors from the hall outward, breadth first over shared walls.
    std::vector<int> connected(cells.size(), -1);
    std::vector<size_t> frontier;
    std::vector<std::pair<size_t, size_t>> doors; // (room, through)

    // The walk starts from every circulation cell, not from the first room.
    //
    // Starting at one room and assuming the rest hang off it fails as soon as
    // the stair core sits between the hall and the back of the plate: the back
    // band touches the core and no room at all, came out unreachable, and the
    // whole building was declined - 120 of 199 declines over the lot corpus,
    // every one a full width room rather than a sliver. The hall and the core
    // are one connected space, and a room that touches either is reached from
    // it directly.
    for (size_t k = 0; k < cells.size(); ++k)
    {
      if (cells[k].hall)
      {
        connected[k] = int(k);
        frontier.push_back(k);
      }
    }

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

    // Repair pass: a room the wide-door walk could not reach gets the best
    // opening it actually has.
    //
    // The walk demands 1.2 m of shared wall, which is a door with room to
    // carry furniture through it. A room whose only shared wall is shorter is
    // not unreachable, it is reached through a narrower opening - and
    // declining the whole building over one of them was 120 of 199 declines
    // across the lot corpus. Watabou's second stage assigns what it can and
    // leaves the rest generic rather than refusing the plan; this is the same
    // principle applied to circulation instead of to purpose.
    //
    // Repeated until nothing more connects, so a chain of narrow rooms is
    // reached through each other rather than only the first of them.
    for (bool progressed = true; progressed;)
    {
      progressed = false;

      for (size_t k = 1; k < cells.size(); ++k)
      {
        if (connected[k] >= 0)
        {
          continue;
        }

        size_t best = 0;
        double widest = 0;

        for (size_t from = 0; from < cells.size(); ++from)
        {
          Shared shared;

          if (from == k || connected[from] < 0 || !sharedEdge(cells[from].rect, cells[k].rect, 1e-6, shared))
          {
            continue;
          }

          if (shared.hi - shared.lo > widest)
          {
            widest = shared.hi - shared.lo;
            best = from;
          }
        }

        // Narrower than this is a gap between two walls, not an opening.
        if (widest < .7)
        {
          continue;
        }

        connected[k] = int(best);
        doors.push_back({k, best});
        progressed = true;
      }
    }

    for (size_t k = 1; k < cells.size(); ++k)
    {
      if (connected[k] < 0 && !cells[k].hall)
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

      if (cell.hall)
      {
        continue;
      }

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

        if (cell.hall || cell.use != RoomUse::Shop || cell.rect.z0 > t + 1e-6 || cell.rect.width() < 2.8)
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
      if (spiral)
      {
        // A spiral is one flight that climbs a whole storey inside its own
        // footprint, so it has no half landing and no second run: the core
        // square is the opening, and the hall in front of it is the landing at
        // the bottom.
        DwellingStair only;
        only.opening = ring(core, y + b.floorRise);
        only.lowerLanding = ring(Rect{core.x0, hall.z0, core.x1, hall.z1}, y);
        only.upperLanding = ring(core, y + b.floorRise);
        only.lowerFloor = f;
        only.risers = risers;
        only.rise = b.floorRise;
        only.width = std::min(flightWidth, core.width() * .5);
        only.privateAccess = b.use == DwellingUse::ShopHouse || b.use == DwellingUse::Cottage;
        p.stairs.push_back(only);

        floor.circulation.push_back(ring(core, y));
        floor.circulationArea += core.area();
      }
      else
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
    // A one-room cottage sleeps its household in the room it cooks in, and has
    // done for most of history. Declining the whole building because no room
    // was labelled Sleeping threw away 51 plans over the lot corpus that were
    // otherwise complete - the label was missing, not the bed. Give the
    // largest room on the ground floor a bed and record it, which is what
    // Watabou's second stage does when a purpose finds no room to sit in:
    // assign what it can and leave the rest generic.
    rewrite::DwellingRoom *largest = nullptr;

    for (rewrite::DwellingRoom &room : p.rooms)
    {
      // Any room on the ground floor, the hall included: on a one-room cottage
      // the only room *is* the hall, and excluding it left 24 plans refused
      // for want of a bed in a house that had exactly one place to put one.
      if (room.floor == 0 && (largest == nullptr || room.usableArea > largest->usableArea))
      {
        largest = &room;
      }
    }

    if (largest != nullptr)
    {
      largest->use = RoomUse::Living;
      p.residentCapacity = std::max(1u, uint32_t(largest->usableArea / 12.));
    }
  }

  if ((b.use == DwellingUse::Cottage || b.use == DwellingUse::ShopHouse || b.use == DwellingUse::Lodging) && !p.residentCapacity)
  {
    return fail("residential programme has no room able to hold a bed");
  }

  if (error != nullptr)
  {
    error->clear();
  }

  out = std::move(p);

  return true;
}

} // namespace procedural::world::generator
