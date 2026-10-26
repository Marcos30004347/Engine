// The public square: what stands on it, and that it stands where it should.
//
// A square is planned as an enclosure, gates, a dais, a monument and the
// trade inside the arcade. Each check here is a question about the finished
// geometry, not about what the planner meant: that every gate is as wide as
// the street that arrives at it, that nothing stands on the dais, that the
// arcade does not run across a gate, that every face the mesher emits is a
// planar polygon the city's triangulator accepts.
#include "procedural/geom3/Prism.hpp"
#include "procedural/world/generator/CityBlockPlanner.hpp"
#include "procedural/world/generator/PlazaPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::world::generator;
namespace rewrite = procedural::world::rewrite;
namespace hierarchy = procedural::world::hierarchy;
using rewrite::Position;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  std::cout << (condition ? "  PASSED " : "  FAILED ") << what << "\n";
  failures += condition ? 0 : 1;
}

double distance(const Position &a, const Position &b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

bool inside(const std::vector<Position> &ring, const Position &p)
{
  bool in = false;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const Position &a = ring[i], &b = ring[j];

    if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x)
    {
      in = !in;
    }
  }

  return in;
}

// Every face is planar, has area, and triangulates.
bool wellFormed(const hierarchy::Solid &solid, std::string &why)
{
  size_t faces = 0;

  for (const auto *group : {&solid.walls, &solid.innerWalls, &solid.slopes, &solid.gables, &solid.soffits, &solid.pillars,
                            &solid.joinery, &solid.decking, &solid.stairs, &solid.timbers, &solid.foundations})
  {
    for (const auto &face : *group)
    {
      ++faces;

      if (face.size() < 3)
      {
        why = "a face with fewer than three corners";

        return false;
      }

      double nx = 0, ny = 0, nz = 0;

      for (size_t k = 0; k < face.size(); ++k)
      {
        const auto &a = face[k], &b = face[(k + 1) % face.size()];
        nx += (a.y - b.y) * (a.z + b.z);
        ny += (a.z - b.z) * (a.x + b.x);
        nz += (a.x - b.x) * (a.y + b.y);
      }

      const double area = std::hypot(nx, std::hypot(ny, nz)) * .5;

      if (!(area > 1e-6))
      {
        why = "a face with no area";

        return false;
      }

      for (const auto &v : face)
      {
        const double off = ((v.x - face[0].x) * nx + (v.y - face[0].y) * ny + (v.z - face[0].z) * nz) / (2. * area);

        if (std::abs(off) > 1e-6)
        {
          why = "a face that is not planar";

          return false;
        }

        if (uint32_t(v.material) == 0)
        {
          why = "a face with no surface";

          return false;
        }
      }

      std::vector<geom3::Vertex> vertices;

      for (const auto &v : face)
      {
        vertices.push_back({v.x, v.y, v.z});
      }

      std::vector<std::array<uint32_t, 3>> triangles;
      std::string error;

      if (!geom3::triangulateFace(vertices, triangles, &error))
      {
        why = "a face the triangulator refuses: " + error;

        return false;
      }
    }
  }

  if (faces == 0)
  {
    why = "no faces at all";

    return false;
  }

  return true;
}

} // namespace

int main()
{
  std::cout << "the public square\n";

  // --- a market square of the town's usual size -----------------------------
  {
    PlazaBrief brief;
    brief.id = 41;
    brief.seed = 7;
    brief.ring = {{0, 0, 0}, {46, 0, 0}, {46, 0, 46}, {0, 0, 46}};

    Ledger ledger;
    rewrite::CityPlazaPlan plan;
    std::string error;
    const bool ok = PlazaPlanner::plan(brief, plan, ledger, &error);

    if (!ok)
    {
      std::cerr << "  " << error << "\n";
    }

    check(ok, "a 46 m square plans");

    for (const auto &note : plan.notes)
    {
      std::cout << "  " << note << "\n";
    }

    check(plan.entries.size() == 4, "with no streets given, every side gets an entry");

    uint32_t gates = 0;
    bool gatesWide = true, gatesOnEntries = true;

    for (const auto &bay : plan.bays)
    {
      if (!bay.gate)
      {
        continue;
      }

      ++gates;
      const double clear = distance(bay.a, bay.b) - bay.pierMetres;
      gatesWide = gatesWide && clear >= brief.gateClearMetres - 1e-6 && bay.apexMetres > bay.springingMetres + 2. && bay.topMetres > bay.apexMetres;
      const Position middle{(bay.a.x + bay.b.x) * .5, 0, (bay.a.z + bay.b.z) * .5};
      gatesOnEntries = gatesOnEntries && bay.entry < plan.entries.size() && distance(middle, plan.entries[bay.entry].at) < .01;
    }

    check(gates == 4, "every entry has a gate");
    check(gatesWide, "every gate is as wide as the street, with an arch and a storey over it");
    check(gatesOnEntries, "every gate is centred on its entry");

    // The arcade: bays along each side in order, meeting pier to pier and
    // never overlapping, every pier inside the square.
    bool contiguous = true, within = true, closed = true;

    for (size_t k = 0; k < plan.bays.size(); ++k)
    {
      const auto &bay = plan.bays[k];
      within = within && inside(brief.ring, bay.a) && inside(brief.ring, bay.b);
      closed = closed && (bay.gate || bay.parapetMetres > .9 || bay.parapetMetres == 0);

      if (k + 1 < plan.bays.size())
      {
        const auto &next = plan.bays[k + 1];
        const bool sameSide = std::abs((next.b.x - next.a.x) * (bay.b.z - bay.a.z) - (next.b.z - next.a.z) * (bay.b.x - bay.a.x)) < 1e-6;

        if (sameSide)
        {
          contiguous = contiguous && distance(bay.b, next.a) < 1e-6;
        }
      }
    }

    check(plan.bays.size() >= 24, "the arcade runs round the square in bays");
    check(contiguous, "bays on one side meet pier to pier");
    check(within, "the arcade stands inside the square");
    check(closed, "every bay is a gate, a closed bay or an open walk");
    check(plan.corners.size() == 4, "a pier at every corner");

    check(plan.terraces.size() == 3, "three steps climb to the dais");
    bool nested = true;

    for (size_t k = 1; k < plan.terraces.size(); ++k)
    {
      nested = nested && plan.terraces[k].level > plan.terraces[k - 1].level &&
               distance(plan.terraces[k].ring.front(), plan.centre) < distance(plan.terraces[k - 1].ring.front(), plan.centre);
    }

    check(nested, "each step is higher and smaller than the one below");
    check(plan.monument != rewrite::CityPlazaMonument::None && plan.monumentRadius > 0, "a monument stands at the centre");
    const double dais = plan.terraces.empty() ? 0 : distance(plan.terraces.back().ring.front(), plan.centre);
    check(plan.monumentRadius + 2. <= dais, "the monument leaves the dais walkable round it");

    bool tradeClear = true, tradeInside = true;

    for (const auto *buildings : {&plan.stalls, &plan.halls})
    {
      for (const auto &ring : *buildings)
      {
        for (const auto &p : ring)
        {
          tradeClear = tradeClear && distance(p, plan.centre) > dais + 1.;
          tradeInside = tradeInside && inside(brief.ring, p);
        }
      }
    }

    // A 46 m square has the depth for houses on every side, and then the
    // houses are its trade: no shambles, no hall.
    check(plan.lots.size() >= 12 && plan.stalls.empty() && plan.halls.empty(), "rows of houses behind the arcade on every side, no shambles");
    bool watch=false;for(const auto &lot:plan.lots)watch |= lot.role=="watch-house";
    check(watch,"large housed square reserves an occupied watch house");
    bool inn = false, pub = false, bakehouse = false, rows = true, lotsInside = true, lotsClear = true, behindWalk = true;
    uint32_t sides = 0;
    std::vector<bool> sideSeen(4, false);

    for (const auto &lot : plan.lots)
    {
      inn = inn || lot.role == "inn";
      pub = pub || lot.role == "pub";
      bakehouse = bakehouse || lot.role == "bakehouse";
      rows = rows && lot.ring.size() == 4;
      sideSeen[lot.side % 4] = true;

      for (const auto &p : lot.ring)
      {
        lotsInside = lotsInside && inside(brief.ring, p);
        lotsClear = lotsClear && distance(p, plan.centre) > dais + 1.;
        // Behind the arcade's walk: at least the inset, the pier and the
        // walk in from the boundary.
        double edge = 1e300;

        for (size_t i = 0; i < brief.ring.size(); ++i)
        {
          const auto &a = brief.ring[i], &c = brief.ring[(i + 1) % brief.ring.size()];
          const double dx = c.x - a.x, dz = c.z - a.z, run = std::hypot(dx, dz);
          const double t = std::max(0., std::min(1., ((p.x - a.x) * dx + (p.z - a.z) * dz) / (run * run)));
          edge = std::min(edge, std::hypot(p.x - a.x - dx * t, p.z - a.z - dz * t));
        }

        behindWalk = behindWalk && edge >= brief.insetMetres + brief.pierMetres * .5 + brief.arcadeWalkMetres - 1e-6;
      }
    }

    for (const bool seen : sideSeen)
    {
      sides += seen ? 1 : 0;
    }

    bool hall = false;

    for (const auto &lot : plan.lots)
    {
      hall = hall || lot.role == "civic-hall";
    }

    check(inn && pub && bakehouse, "an inn, a pub and a bakehouse stand among the shop-houses");
    check(hall, "a civic hall fronts the square");

    bool gatehouses = true;

    for (const auto &bay : plan.bays)
    {
      gatehouses = gatehouses && (!bay.gate || bay.topMetres >= bay.apexMetres + 2.5);
    }

    check(gatehouses, "every gate carries a storey over its arch");
    check(rows && sides == 4, "the rows are rectangles, on all four sides");
    check(lotsInside && behindWalk, "every house stands inside the square, behind the arcade's walk");
    check(lotsClear, "no house reaches the dais");

    // No two lots overlap: rectangles, so their centres are at least the
    // smaller half-width apart along the row.
    bool disjoint = true;

    for (size_t i = 0; i < plan.lots.size(); ++i)
    {
      for (size_t j = i + 1; j < plan.lots.size(); ++j)
      {
        const auto &p = plan.lots[i].ring, &q = plan.lots[j].ring;
        Position pc{}, qc{};

        for (size_t k = 0; k < 4; ++k)
        {
          pc.x += p[k].x * .25; pc.z += p[k].z * .25;
          qc.x += q[k].x * .25; qc.z += q[k].z * .25;
        }

        disjoint = disjoint && !inside(p, qc) && !inside(q, pc);
      }
    }

    check(disjoint, "no two houses stand on the same ground");

    // The gates' lanes run through the rows.
    bool lanesClear = true;

    for (const auto &bay : plan.bays)
    {
      if (!bay.gate)
      {
        continue;
      }

      const Position middle{(bay.a.x + bay.b.x) * .5, 0, (bay.a.z + bay.b.z) * .5};
      const Position deep{middle.x + bay.inward.x * 7., 0, middle.z + bay.inward.z * 7.};

      for (const auto &lot : plan.lots)
      {
        lanesClear = lanesClear && !inside(lot.ring, deep);
      }
    }

    check(lanesClear, "a gate's lane runs through the row to the square");

    // Bays with a house behind them are the house's covered walk: open.
    bool openBehind = true;

    for (const auto &bay : plan.bays)
    {
      const Position probe{(bay.a.x + bay.b.x) * .5 + bay.inward.x * (brief.arcadeWalkMetres + 1.), 0, (bay.a.z + bay.b.z) * .5 + bay.inward.z * (brief.arcadeWalkMetres + 1.)};
      bool house = false;

      for (const auto &lot : plan.lots)
      {
        house = house || inside(lot.ring, probe);
      }

      openBehind = openBehind && (!house || bay.parapetMetres == 0);
    }

    check(openBehind, "the arcade is open where a house stands behind it");
    check(tradeClear, "nothing of the trade stands on the dais");
    check(tradeInside, "the trade stands inside the square");

    // The same square without houses keeps its shambles and hall.
    {
      auto open = brief;
      open.houses = false;
      rewrite::CityPlazaPlan bare;
      Ledger bareLedger;
      check(PlazaPlanner::plan(open, bare, bareLedger, &error) && bare.stalls.size() >= 4 && bare.halls.size() == 1 && bare.lots.empty(),
            "without houses, shambles down one side and a hall across from them");
    }

    // No stall under a gate's approach.
    bool approachesClear = true;

    for (const auto &bay : plan.bays)
    {
      if (!bay.gate)
      {
        continue;
      }

      const Position middle{(bay.a.x + bay.b.x) * .5, 0, (bay.a.z + bay.b.z) * .5};

      for (const auto &stall : plan.stalls)
      {
        for (const auto &p : stall)
        {
          approachesClear = approachesClear && distance(p, middle) > 2.;
        }
      }
    }

    check(approachesClear, "the gates' approaches are clear of stalls");

    hierarchy::Solid mesh;
    meshPlaza(plan, mesh);
    std::string why;
    const bool formed = wellFormed(mesh, why);

    if (!formed)
    {
      std::cerr << "  " << why << "\n";
    }

    check(formed, "every face of the square is a planar polygon the triangulator accepts");
    check(!mesh.soffits.empty() && !mesh.pillars.empty() && !mesh.decking.empty() && !mesh.slopes.empty(), "the mesh has arches, piers, paving and tiled roofs");

    rewrite::CityPlazaPlan again;
    Ledger ledgerAgain;
    PlazaPlanner::plan(brief, again, ledgerAgain, &error);
    check(again.bays.size() == plan.bays.size() && again.monument == plan.monument && again.stalls.size() == plan.stalls.size(), "the same brief gives the same square");
  }

  // --- a square with streets arriving off-centre, on a skewed outline -------
  {
    PlazaBrief brief;
    brief.id = 42;
    brief.seed = 3;
    brief.ring = {{0, 0, 0}, {58, 0, 2}, {62, 0, 40}, {-3, 0, 38}};
    brief.entries = {{{14, 0, .5}, {0, 0, 1}, 6.}, {{60, 0, 20}, {-1, 0, 0}, 4.}, {{40, 0, 39}, {0, 0, -1}, 5.}};

    Ledger ledger;
    rewrite::CityPlazaPlan plan;
    std::string error;
    const bool ok = PlazaPlanner::plan(brief, plan, ledger, &error);

    if (!ok)
    {
      std::cerr << "  " << error << "\n";
    }

    check(ok, "a skewed square with three streets plans");

    uint32_t gates = 0;
    bool widths = true;

    for (const auto &bay : plan.bays)
    {
      if (bay.gate)
      {
        ++gates;
        widths = widths && std::abs(distance(bay.a, bay.b) - bay.pierMetres - brief.entries[bay.entry].widthMetres) < 1e-6;
      }
    }

    check(gates == 3, "one gate per street");
    check(widths, "each gate takes its street's width");

    bool overlapFree = true;

    for (size_t i = 0; i < plan.bays.size(); ++i)
    {
      for (size_t j = i + 1; j < plan.bays.size(); ++j)
      {
        const auto &p = plan.bays[i], &q = plan.bays[j];
        // Only bays on one line can overlap; bays round a corner may have
        // their middles closer than their half-lengths add up to.
        const auto offLine = [&](const Position &r) {
          return std::abs((p.b.x - p.a.x) * (r.z - p.a.z) - (p.b.z - p.a.z) * (r.x - p.a.x)) > 1e-6 * distance(p.a, p.b);
        };

        if (offLine(q.a) || offLine(q.b))
        {
          continue;
        }

        const Position pm{(p.a.x + p.b.x) * .5, 0, (p.a.z + p.b.z) * .5}, qm{(q.a.x + q.b.x) * .5, 0, (q.a.z + q.b.z) * .5};
        const bool apart = distance(pm, qm) >= (distance(p.a, p.b) + distance(q.a, q.b)) * .5 - 1e-6;

        if (!apart)
        {
          std::cout << "  bays " << i << " and " << j << " overlap: (" << p.a.x << "," << p.a.z << ")-(" << p.b.x << "," << p.b.z
                    << ") gate " << p.gate << " and (" << q.a.x << "," << q.a.z << ")-(" << q.b.x << "," << q.b.z << ") gate " << q.gate << "\n";
        }

        overlapFree = overlapFree && apart;
      }
    }

    check(overlapFree, "no two bays overlap");
  }

  // --- a small square: no dais, no trade, still a gate ----------------------
  {
    PlazaBrief brief;
    brief.id = 43;
    brief.ring = {{0, 0, 0}, {14, 0, 0}, {14, 0, 13}, {0, 0, 13}};

    Ledger ledger;
    rewrite::CityPlazaPlan plan;
    std::string error;
    check(PlazaPlanner::plan(brief, plan, ledger, &error), "a small square plans");
    check(plan.terraces.empty(), "a small square has no dais");
    check(plan.stalls.empty() && plan.halls.empty(), "a small square has no shambles and no hall");

    uint32_t gates = 0;

    for (const auto &bay : plan.bays)
    {
      gates += bay.gate ? 1 : 0;
    }

    check(gates >= 1, "a small square still has a way in");

    hierarchy::Solid mesh;
    meshPlaza(plan, mesh);
    std::string why;
    check(wellFormed(mesh, why), "the small square meshes");
  }

  // A spacious market has roofed trading rooms, without consuming its routes.
  {
    PlazaBrief brief;
    brief.id=101; brief.ring={{0,0,0},{100,0,0},{100,0,90},{0,0,90}};
    brief.entries={{{50,0,0},{0,0,1},6},{{100,0,45},{-1,0,0},6},
                   {{50,0,90},{0,0,-1},6},{{0,0,45},{1,0,0},6}};
    Ledger ledger; rewrite::CityPlazaPlan plan; std::string why;
    check(PlazaPlanner::plan(brief,plan,ledger,&why),"large civic market plans");
    check(!plan.pavilions.empty() && plan.pavilions.size()<=3,"large square adds bounded roofed market pavilions");
    bool clear=true;
    for(const auto &p:plan.pavilions) {
      clear &= inside(plan.ring,p.centre);
      for(const auto &entry:plan.entries) {
        const double dx=plan.centre.x-entry.at.x,dz=plan.centre.z-entry.at.z;
        const double t=std::clamp(((p.centre.x-entry.at.x)*dx+(p.centre.z-entry.at.z)*dz)/(dx*dx+dz*dz),0.,1.);
        clear &= distance(p.centre,{entry.at.x+dx*t,0,entry.at.z+dz*t})>4.2+entry.widthMetres*.5;
      }
    }
    check(!plan.gardens.empty(),"large square reserves planted seating islands");
    bool gardenClear=true;
    for(const auto &garden:plan.gardens) {
      gardenClear &= inside(plan.ring,garden);
      gardenClear &= distance(garden,plan.centre)>distance(plan.terraces.front().ring.front(),plan.centre)+2.4;
      for(const auto &p:plan.pavilions)gardenClear &= distance(garden,p.centre)>6.6;
      for(const auto &entry:plan.entries) {
        const double dx=plan.centre.x-entry.at.x,dz=plan.centre.z-entry.at.z;
        const double t=std::clamp(((garden.x-entry.at.x)*dx+(garden.z-entry.at.z)*dz)/(dx*dx+dz*dz),0.,1.);
        gardenClear &= distance(garden,{entry.at.x+dx*t,0,entry.at.z+dz*t})>2.4+entry.widthMetres*.5;
      }
    }
    check(gardenClear,"seating islands clear the central steps, pavilion roofs and gate approaches");
    check(clear,"pavilion roofs leave the gate-to-centre walking routes clear");
    hierarchy::Solid mesh; meshPlaza(plan,mesh);
    check(wellFormed(mesh,why),"pavilion roofs, soffits, columns and paving triangulate");
    check(!mesh.soffits.empty(),"covered pavilions have closed undersides");
  }

  // --- through the block planner ---------------------------------------------
  {
    CityBlockPlanInput in;
    in.id = 9;
    in.plaza = true;
    in.boundary = {{0, 0, 0}, {52, 0, 0}, {54, 0, 40}, {1, 0, 40}};
    in.streetEdges = {true, true, true, false};

    CityBlockPlan block;
    std::string error;
    const bool ok = planCityBlock(in, block, &error);

    if (!ok)
    {
      std::cerr << "  " << error << "\n";
    }

    check(ok, "a plaza block plans through the block planner");
    check(block.plaza.bays.size() > 8 && block.plaza.entries.size() == 3, "the block's street edges become the square's gates");
    bool same = block.stalls.size() == block.plaza.stalls.size() && block.halls.size() == block.plaza.halls.size() && !block.plaza.lots.empty();

    for (size_t i = 0; same && i < block.stalls.size(); ++i)
    {
      same = block.stalls[i].size() == block.plaza.stalls[i].size();

      for (size_t k = 0; same && k < block.stalls[i].size(); ++k)
      {
        same = distance(block.stalls[i][k], block.plaza.stalls[i][k]) < 1e-9;
      }
    }

    check(same, "the block carries the square's houses, stalls and hall");
    check(block.paving.size() == 1 && block.lots.empty(), "a plaza block is paved through and has no lots");
  }

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");

  return failures == 0 ? 0 : 1;
}
