#include "CompoundPlanner.hpp"

#include "DwellingPlanner.hpp"
#include "PatchTiling.hpp"
#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {

using rewrite::DwellingBrief;
using rewrite::DwellingUse;

// Compound-local frame: x along the longest precinct edge, z inward.
struct Frame
{
  Position origin;
  double ux = 1, uz = 0;

  Position toLocal(Position p) const
  {
    return {(p.x - origin.x) * ux + (p.z - origin.z) * uz, 0, -(p.x - origin.x) * uz + (p.z - origin.z) * ux};
  }

  Position toWorld(double x, double z) const
  {
    return {origin.x + ux * x - uz * z, 0, origin.z + uz * x + ux * z};
  }
};

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
};

bool rectInside(const ConvexRing &ring, const Rect &r)
{
  for (auto p : {Position{r.x0, 0, r.z0}, Position{r.x1, 0, r.z0}, Position{r.x1, 0, r.z1}, Position{r.x0, 0, r.z1}})
  {
    if (!strictlyInsideConvex(ring, p) && !onRing(ring, p, 1e-6))
    {
      return false;
    }
  }

  return true;
}

// Largest axis-aligned rectangle inside a convex ring by a bounded search
// over anchored candidates; exact enough for a compound footprint.
Rect inscribedRectangle(const ConvexRing &ring)
{
  double x0 = ring[0].x, x1 = x0, z0 = ring[0].z, z1 = z0;

  for (auto p : ring)
  {
    x0 = std::min(x0, p.x);
    x1 = std::max(x1, p.x);
    z0 = std::min(z0, p.z);
    z1 = std::max(z1, p.z);
  }

  Rect best;
  double bestArea = 0;
  const int steps = 14;

  for (int a = 0; a <= steps; ++a)
  {
    for (int b = a + 1; b <= steps; ++b)
    {
      for (int c = 0; c <= steps; ++c)
      {
        for (int d = c + 1; d <= steps; ++d)
        {
          Rect r{x0 + (x1 - x0) * a / steps, z0 + (z1 - z0) * c / steps, x0 + (x1 - x0) * b / steps, z0 + (z1 - z0) * d / steps};
          const double area = r.width() * r.depth();

          if (area > bestArea && rectInside(ring, r))
          {
            best = r;
            bestArea = area;
          }
        }
      }
    }
  }

  return best;
}

} // namespace

bool planCompound(const CompoundBrief &b, const std::function<double(double, double)> &terrain, CompoundPlan &out, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  if (!b.id || b.precinct.size() < 3 || b.precinct.size() > 32 || !terrain)
  {
    return fail("compound needs an id, a convex precinct and a terrain callback");
  }

  if (signedArea(b.precinct) < 150)
  {
    return fail("precinct too small for a compound");
  }

  // Frame along the longest edge.
  size_t longest = 0;

  for (size_t i = 1; i < b.precinct.size(); ++i)
  {
    if (length(b.precinct[i], b.precinct[(i + 1) % b.precinct.size()]) > length(b.precinct[longest], b.precinct[(longest + 1) % b.precinct.size()]))
    {
      longest = i;
    }
  }

  Frame frame;
  frame.origin = b.precinct[longest];
  const auto next = b.precinct[(longest + 1) % b.precinct.size()];
  const double edge = length(frame.origin, next);
  frame.ux = (next.x - frame.origin.x) / edge;
  frame.uz = (next.z - frame.origin.z) / edge;

  ConvexRing local;

  for (auto p : b.precinct)
  {
    local.push_back(frame.toLocal(p));
  }

  if (signedArea(local) < 0)
  {
    std::reverse(local.begin(), local.end());
  }

  const Rect box = inscribedRectangle(local);

  if (box.width() < 12 || box.depth() < 10)
  {
    return fail("precinct holds no usable footprint (needs at least 12 by 10 metres)");
  }

  const Position approach = frame.toLocal(b.approach);

  CompoundPlan plan;
  plan.id = b.id;
  plan.kind = b.kind;
  plan.precinct = b.precinct;

  auto ringOf = [&](const Rect &r) {
    return ConvexRing{frame.toWorld(r.x0, r.z0), frame.toWorld(r.x1, r.z0), frame.toWorld(r.x1, r.z1), frame.toWorld(r.x0, r.z1)};
  };

  // Which side of the footprint faces the approach: 0 south (z0), 1 east, 2 north, 3 west.
  const double toSouth = std::abs(approach.z - box.z0), toNorth = std::abs(approach.z - box.z1), toWest = std::abs(approach.x - box.x0), toEast = std::abs(approach.x - box.x1);
  const int gateSide = std::min({toSouth, toEast, toNorth, toWest}) == toSouth ? 0 : std::min({toEast, toNorth, toWest}) == toEast ? 1 : std::min(toNorth, toWest) == toNorth ? 2 : 3;

  auto sideMidpoint = [&](const Rect &r, int side) {
    switch (side)
    {
    case 0:
      return Position{(r.x0 + r.x1) * .5, 0, r.z0};
    case 1:
      return Position{r.x1, 0, (r.z0 + r.z1) * .5};
    case 2:
      return Position{(r.x0 + r.x1) * .5, 0, r.z1};
    default:
      return Position{r.x0, 0, (r.z0 + r.z1) * .5};
    }
  };

  auto addWing = [&](const std::string &role, const Rect &r, DwellingUse use, uint32_t floors, double height, int doorSide) {
    CompoundWing wing;
    wing.role = role;
    wing.brief.id = b.id * 16 + plan.wings.size() + 1;
    wing.brief.seed = rewrite::featureSeed(b.seed, 4, b.id, plan.wings.size());
    wing.brief.use = use;
    wing.brief.floors = floors;
    wing.brief.availableHeight = height;
    wing.brief.floorRise = std::min(3.4, std::max(2.4, height / floors));
    wing.brief.foundation = b.foundation;
    wing.brief.shell = ringOf(r);
    wing.brief.entrance = frame.toWorld(sideMidpoint(r, doorSide).x, sideMidpoint(r, doorSide).z);
    wing.planned = planDwelling(wing.brief, wing.plan, &wing.note);

    if (wing.planned)
    {
      ++plan.plannedWings;
    }
    else
    {
      ledger.add(LedgerKind::Rejected, b.id, "compound " + std::to_string(b.id) + " " + role + ": " + wing.note);
    }

    plan.wings.push_back(std::move(wing));
  };

  auto wallRing = [&](const Rect &r, double thickness, double height, int openSide, double openingWidth) {
    const ConvexRing ring = ringOf(r);
    plan.enclosure = ring;

    for (size_t k = 0; k < 4; ++k)
    {
      const Position a = ring[k], c = ring[(k + 1) % 4];

      if (int(k) == openSide)
      {
        // Gate: split the side around a centred opening.
        const double len = length(a, c);
        const double t0 = std::max(0., .5 - openingWidth * .5 / len), t1 = std::min(1., .5 + openingWidth * .5 / len);
        const Position g0{a.x + (c.x - a.x) * t0, 0, a.z + (c.z - a.z) * t0}, g1{a.x + (c.x - a.x) * t1, 0, a.z + (c.z - a.z) * t1};
        plan.walls.push_back({a, g0, thickness, height, false});
        plan.walls.push_back({g0, g1, thickness, height, true});
        plan.walls.push_back({g1, c, thickness, height, false});
        plan.gate = {(g0.x + g1.x) * .5, 0, (g0.z + g1.z) * .5};
      }
      else
      {
        plan.walls.push_back({a, c, thickness, height, false});
      }
    }
  };

  const double wallInset = 1.0;
  Rect inner{box.x0 + wallInset, box.z0 + wallInset, box.x1 - wallInset, box.z1 - wallInset};

  switch (b.kind)
  {
  case CompoundKind::Castle: {
    if (inner.width() < 24 || inner.depth() < 20)
    {
      return fail("castle precinct needs at least 24 by 20 metres inside the wall");
    }

    wallRing(inner, 1.8, 7, gateSide, 4);

    // Corner towers as octagons.
    for (auto corner : {Position{inner.x0, 0, inner.z0}, Position{inner.x1, 0, inner.z0}, Position{inner.x1, 0, inner.z1}, Position{inner.x0, 0, inner.z1}})
    {
      CompoundTower tower;
      tower.height = 10;

      for (int k = 0; k < 8; ++k)
      {
        const double angle = 6.283185307179586 * k / 8 + .3926990816987241;
        tower.footprint.push_back(frame.toWorld(corner.x + std::cos(angle) * 3.2, corner.z + std::sin(angle) * 3.2));
      }

      plan.towers.push_back(tower);
    }

    // Gatehouse straddling the gate.
    {
      const Position g = frame.toLocal(plan.gate);
      const bool horizontal = gateSide == 0 || gateSide == 2;
      Rect house = horizontal ? Rect{g.x - 4, g.z - 3, g.x + 4, g.z + 3} : Rect{g.x - 3, g.z - 4, g.x + 3, g.z + 4};
      CompoundTower gatehouse;
      gatehouse.height = 9;
      gatehouse.footprint = ringOf(house);
      plan.towers.push_back(gatehouse);
    }

    // Keep in the corner farthest from the gate, preferring higher ground.
    const double keepW = std::min(14., inner.width() * .4), keepD = std::min(12., inner.depth() * .4);
    Rect keep;
    double bestScore = -1e300;

    for (auto corner :
         {Rect{inner.x0 + 2.5, inner.z0 + 2.5, inner.x0 + 2.5 + keepW, inner.z0 + 2.5 + keepD},
          Rect{inner.x1 - 2.5 - keepW, inner.z0 + 2.5, inner.x1 - 2.5, inner.z0 + 2.5 + keepD},
          Rect{inner.x1 - 2.5 - keepW, inner.z1 - 2.5 - keepD, inner.x1 - 2.5, inner.z1 - 2.5},
          Rect{inner.x0 + 2.5, inner.z1 - 2.5 - keepD, inner.x0 + 2.5 + keepW, inner.z1 - 2.5}})
    {
      const Position centre = frame.toWorld((corner.x0 + corner.x1) * .5, (corner.z0 + corner.z1) * .5);
      const Position g = frame.toLocal(plan.gate);
      const double score = std::hypot((corner.x0 + corner.x1) * .5 - g.x, (corner.z0 + corner.z1) * .5 - g.z) + 3 * terrain(centre.x, centre.z);

      if (score > bestScore)
      {
        bestScore = score;
        keep = corner;
      }
    }

    addWing("keep", keep, DwellingUse::Hall, 2, 9, gateSide == 0 || gateSide == 2 ? (keep.z0 > inner.z0 + inner.depth() * .5 ? 0 : 2) : (keep.x0 > inner.x0 + inner.width() * .5 ? 3 : 1));

    // Hall range along the long wall away from the keep; store range opposite.
    const bool keepLeft = (keep.x0 + keep.x1) * .5 < (inner.x0 + inner.x1) * .5;
    const bool keepSouth = (keep.z0 + keep.z1) * .5 < (inner.z0 + inner.z1) * .5;
    Rect hall = keepSouth ? Rect{inner.x0 + 4, inner.z1 - 2.5 - 8, inner.x1 - 4, inner.z1 - 2.5} : Rect{inner.x0 + 4, inner.z0 + 2.5, inner.x1 - 4, inner.z0 + 2.5 + 8};

    if (gateSide == (keepSouth ? 2 : 0))
    {
      hall = keepLeft ? Rect{inner.x1 - 2.5 - 8, inner.z0 + 4, inner.x1 - 2.5, inner.z1 - 4} : Rect{inner.x0 + 2.5, inner.z0 + 4, inner.x0 + 2.5 + 8, inner.z1 - 4};
    }

    if (hall.width() >= 12 && hall.depth() >= 6)
    {
      addWing("hall", hall, DwellingUse::Hall, 1, 6, keepSouth ? 0 : 2);
    }

    Rect store = keepLeft ? Rect{inner.x1 - 2.5 - 6, keepSouth ? inner.z0 + 2.5 : inner.z1 - 2.5 - 12, inner.x1 - 2.5, keepSouth ? inner.z0 + 2.5 + 12 : inner.z1 - 2.5}
                          : Rect{inner.x0 + 2.5, keepSouth ? inner.z0 + 2.5 : inner.z1 - 2.5 - 12, inner.x0 + 2.5 + 6, keepSouth ? inner.z0 + 2.5 + 12 : inner.z1 - 2.5};

    if (store.width() >= 5 && store.depth() >= 6)
    {
      addWing("stables", store, DwellingUse::Warehouse, 1, 5, keepLeft ? 3 : 1);
    }

    plan.courts.push_back(ringOf(inner)); // Bailey: the enclosure minus wings (accounting only).
    break;
  }
  case CompoundKind::Manor: {
    if (inner.width() < 16 || inner.depth() < 14)
    {
      return fail("manor precinct needs at least 16 by 14 metres inside the garden wall");
    }

    wallRing(inner, .5, 2.5, gateSide, 3);

    // Hall wing across the side opposite the gate, service wing at right angle: an L around the court.
    const bool gateHorizontal = gateSide == 0 || gateSide == 2;
    // The hall wing is 11 m deep: entrance hall, dog-leg stair and a room band behind at a 3.4 m floor rise.
    const double hallDepth = std::min(11., (gateHorizontal ? inner.depth() : inner.width()) - 8);
    Rect hall = gateHorizontal ? (gateSide == 0 ? Rect{inner.x0 + 1.5, inner.z1 - 1.5 - hallDepth, inner.x1 - 1.5, inner.z1 - 1.5} : Rect{inner.x0 + 1.5, inner.z0 + 1.5, inner.x1 - 1.5, inner.z0 + 1.5 + hallDepth})
                               : (gateSide == 3 ? Rect{inner.x1 - 1.5 - hallDepth, inner.z0 + 1.5, inner.x1 - 1.5, inner.z1 - 1.5} : Rect{inner.x0 + 1.5, inner.z0 + 1.5, inner.x0 + 1.5 + hallDepth, inner.z1 - 1.5});
    addWing("hall", hall, DwellingUse::Hall, 2, 7, gateHorizontal ? (gateSide == 0 ? 0 : 2) : (gateSide == 3 ? 3 : 1));

    Rect service = gateHorizontal ? Rect{inner.x0 + 1.5, gateSide == 0 ? inner.z0 + 1.5 + 4 : hall.z1, inner.x0 + 1.5 + 7, gateSide == 0 ? hall.z0 : inner.z1 - 1.5 - 4}
                                  : Rect{gateSide == 3 ? inner.x0 + 1.5 + 4 : hall.x1, inner.z0 + 1.5, gateSide == 3 ? hall.x0 : inner.x1 - 1.5 - 4, inner.z0 + 1.5 + 7};

    if (service.width() >= 5 && service.depth() >= 5)
    {
      addWing("service", service, DwellingUse::Warehouse, 1, 4.5, gateHorizontal ? 1 : 2);
    }

    plan.courts.push_back(ringOf(inner));
    break;
  }
  case CompoundKind::Church: {
    if (inner.width() < 14 || inner.depth() < 10)
    {
      return fail("church precinct needs at least 14 by 10 metres");
    }

    wallRing(inner, .4, 1.2, gateSide, 3);
    const double naveW = std::min(inner.width() - 8, 24.), naveD = std::min(inner.depth() - 4, 10.);
    Rect nave{(inner.x0 + inner.x1) * .5 - naveW * .5 + 3, (inner.z0 + inner.z1) * .5 - naveD * .5, (inner.x0 + inner.x1) * .5 + naveW * .5 + 3, (inner.z0 + inner.z1) * .5 + naveD * .5};
    addWing("nave", nave, DwellingUse::Hall, 1, 9, gateSide == 0 ? 0 : gateSide == 2 ? 2 : 3);
    Rect tower{nave.x0 - 6, nave.z0, nave.x0, nave.z0 + 6};

    if (tower.x0 >= inner.x0 && tower.z1 <= inner.z1)
    {
      CompoundTower belfry;
      belfry.height = 14;
      belfry.footprint = ringOf(tower);
      plan.towers.push_back(belfry);
    }

    plan.courts.push_back(ringOf(inner));
    break;
  }
  default: {
    if (inner.width() < 12 || inner.depth() < 10)
    {
      return fail("hall precinct needs at least 12 by 10 metres");
    }

    Rect hall{inner.x0 + 1, inner.z0 + 1, inner.x1 - 1, std::min(inner.z1 - 1, inner.z0 + 1 + 12)};
    addWing("hall", hall, DwellingUse::Hall, 2, 7, gateSide == 2 ? 2 : 0);
    plan.courts.push_back(ringOf(inner));
    plan.gate = b.approach;
    break;
  }
  }

  plan.solved = plan.plannedWings > 0 && plan.plannedWings == plan.wings.size();

  if (error != nullptr)
  {
    error->clear();
  }

  out = std::move(plan);

  return true;
}

} // namespace procedural::world::generator
