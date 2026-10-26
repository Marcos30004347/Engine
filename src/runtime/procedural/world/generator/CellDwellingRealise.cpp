#include "DwellingStylePlanner.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"

#include "procedural/world/generator/CellDwellingRoof.hpp"
#include "procedural/world/generator/CellDwellingStairs.hpp"
#include "procedural/world/generator/DwellingStairs.hpp"

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom3/Prism.hpp"

#include <map>

#include <algorithm>
#include <array>
#include <cmath>

namespace procedural::world::generator {

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

// A maximal stretch of contour running one way: one wall of the building.
//
// The unit a jetty and a balcony are both decided on, because both are
// properties of a WALL and not of a corner or of a lattice step. A contour of
// forty edges is six or eight walls, and those are what a builder would point
// at.
struct WallRun
{
  geom2::Dir dir = geom2::Dir::North;
  uint32_t startNode = geom2::kNoNode;
  uint32_t endNode = geom2::kNoNode;
  std::vector<uint32_t> edges;

  // Where the run's line sits in the grid's frame: the column for a run going
  // north or south, the row for one going east or west.
  bool horizontal = false;
  double line = 0;

  double project = 0; // Cells this run is pushed outward.
};

std::vector<WallRun> wallRuns(const CellGrid &grid, const Chain &contour)
{
  std::vector<WallRun> runs;

  if (contour.empty())
  {
    return runs;
  }

  const size_t n = contour.size();
  const size_t start = geom2::firstCorner(grid, contour);

  for (size_t step = 0; step < n; ++step)
  {
    const size_t k = (start + step) % n;
    const uint32_t e = contour[k];
    const geom2::Dir dir = grid.decode(e).dir;

    if (runs.empty() || runs.back().dir != dir)
    {
      WallRun run;
      run.dir = dir;
      run.startNode = grid.decode(e).node;
      runs.push_back(std::move(run));
    }

    runs.back().edges.push_back(e);
    runs.back().endNode = grid.edgeEndNode(e);
  }

  for (WallRun &run : runs)
  {
    run.horizontal = run.dir == geom2::Dir::East || run.dir == geom2::Dir::West;
    run.line = run.horizontal ? double(grid.nodeRow(run.startNode)) : double(grid.nodeColumn(run.startNode));
  }

  return runs;
}

// Which way is out of the building, for a run. The contour is walked with the
// interior on its right, so out is the quarter turn the other way.
void outwardStep(const geom2::Dir dir, double &dj, double &di)
{
  const geom2::Dir out = ccw(dir);
  dj = double(geom2::deltaJ(out));
  di = double(geom2::deltaI(out));
}

// The ring a set of runs makes once each has been pushed out by its own amount.
//
// Every run is axis-aligned in the grid's frame and consecutive runs turn a
// right angle, so a corner is simply the column of the vertical run and the row
// of the horizontal one. No intersection arithmetic, and no chance of the
// nearly-parallel case that a general variable offset has to worry about.
std::vector<rewrite::Position> runRing(const CellFrame &frame, const std::vector<WallRun> &runs, const double y)
{
  std::vector<rewrite::Position> out;
  const size_t n = runs.size();

  if (n < 4)
  {
    return out;
  }

  std::vector<double> lines(n, 0);

  for (size_t k = 0; k < n; ++k)
  {
    double dj = 0;
    double di = 0;
    outwardStep(runs[k].dir, dj, di);
    lines[k] = runs[k].line + (runs[k].horizontal ? di : dj) * runs[k].project;
  }

  for (size_t k = 0; k < n; ++k)
  {
    const size_t previous = (k + n - 1) % n;
    const double u = runs[k].horizontal ? lines[previous] : lines[k];
    const double v = runs[k].horizontal ? lines[k] : lines[previous];

    double x = 0;
    double z = 0;
    geom2::framePoint(frame, u, v, x, z);
    out.push_back(rewrite::Position{x, y, z});
  }

  return out;
}

std::vector<rewrite::Position> plainRing(const CellGrid &grid, const CellFrame &frame, const Chain &loop, const double y)
{
  std::vector<rewrite::Position> out;

  for (const uint32_t n : geom2::mergeCollinear(grid, loop))
  {
    double x = 0;
    double z = 0;
    geom2::nodePoint(grid, frame, n, x, z);
    out.push_back(rewrite::Position{x, y, z});
  }

  return out;
}

double runLengthMetres(const CellGrid &grid, const CellFrame &frame, const WallRun &run)
{
  return double(run.edges.size()) * frame.cellMetres;
}

// Plan attached upper-floor chambers before the structural/mesh passes. These
// are hollow rooms with access, not solid decorative cylinders on the facade.
void wallTurrets(const CellHouse &house, const CellDwellingSettings &settings, rewrite::DwellingPlan &plan)
{
  if (house.building != BuildingClass::Castle || house.floors.size() < 2) return;
  using rewrite::Position;
  const auto polygon = [](const std::vector<Position> &ring) {
    geom2::Polygon p;
    for (auto v : ring) p.outer.push_back({geom2::toUnit(v.x), geom2::toUnit(v.z)});
    if(!geom2::counterClockwise(p.outer))geom2::reverse(p.outer);
    return p;
  };
  const auto inside = [&](const std::vector<Position> &ring, Position p) {
    return geom2::contains(polygon(ring), {geom2::toUnit(p.x), geom2::toUnit(p.z)});
  };
  const size_t originalWalls = plan.walls.size();
  uint64_t nextWall = 1, nextRoom = 1;
  for (const auto &w : plan.walls) nextWall = std::max(nextWall, w.id + 1);
  for (const auto &r : plan.rooms) nextRoom = std::max(nextRoom, r.id + 1);
  // Highest occupied levels first; opposite elevations are preferred by the
  // separation test. Stable wall order makes the choice seed reproducible.
  for (size_t level = plan.floors.size(); level-- > 0 && plan.wallTurrets.size() < 2;)
  for (size_t wi = 0; wi < originalWalls && plan.wallTurrets.size() < 2; ++wi)
  {
    const auto host = plan.walls[wi];
    if (host.floor != level || host.a.y < 2.5 || !host.exterior ||
        host.kind != rewrite::WallKind::Solid || host.height < 2.5) continue;
    if(std::any_of(plan.doors.begin(),plan.doors.end(),[&](const auto &d){return d.wall==host.id;})) continue;
    const double length = std::hypot(host.b.x-host.a.x, host.b.z-host.a.z);
    if (length < 2.4) continue;
    const double ux=(host.b.x-host.a.x)/length, uz=(host.b.z-host.a.z)/length;
    double nx=uz, nz=-ux;
    const Position mid{(host.a.x+host.b.x)*.5,host.a.y,(host.a.z+host.b.z)*.5};
    const rewrite::DwellingRoom *owner=nullptr;
    for (const auto &r : plan.rooms) if (r.floor==level) {
      if (inside(r.boundary,{mid.x-nx*.3,mid.y,mid.z-nz*.3})) {owner=&r;break;}
      if (inside(r.boundary,{mid.x+nx*.3,mid.y,mid.z+nz*.3})) {nx=-nx;nz=-nz;owner=&r;break;}
    }
    if (!owner) continue;
    const uint64_t hostRoom=owner->id;
    const double half=std::min(1.35,length*.44), depth=1.7, base=host.a.y;
    const double accessWidth = settings.doorWidthMetres;
    if (accessWidth > 2 * half - .2) continue;
    const double height=std::min(host.height,house.gridClass.storeyMetres);
    const auto at=[&](double u,double v,double y){return Position{mid.x+ux*u+nx*v,y,mid.z+uz*u+nz*v};};
    bool clear=true;
    // Keep the whole chamber inside buildable land and outside every existing
    // storey. Sampling the full envelope also protects re-entrant corners.
    for (double u=-half;u<=half+.01;u+=half/4)
    for (double v=.25;v<=depth+.01;v+=.24) {
      auto p=at(u,v,base);const auto c=geom2::cellAtPoint(house.grid,house.frame,p.x,p.z);
      if (c==geom2::kNoCell || !house.grid.contains(house.site.buildable,c)) clear=false;
      for (const auto &s:plan.storeys) if (inside(s.outer,p)) clear=false;
    }
    for (const auto &t:plan.wallTurrets)
      for(auto p:t.footprint) if(std::hypot(p.x-mid.x,p.z-mid.z)<5.) clear=false;
    // Do not build a corbel over an opening, a porch, stair or roof.
    const auto near=[&](Position p,double margin){
      const double u=(p.x-mid.x)*ux+(p.z-mid.z)*uz;
      const double v=(p.x-mid.x)*nx+(p.z-mid.z)*nz;
      return std::abs(u)<half+margin && v>-.35-margin && v<depth+margin;
    };
    for (const auto &o:plan.openings) if(o.wall!=host.id &&
      o.a.y+o.top>base-1.3 && o.a.y+o.base<base+height+1.5 &&
      (near(o.a,.3)||near(o.b,.3))) clear=false;
    const auto envelope=polygon({at(-half,.02,base),at(half,.02,base),
                                 at(half,depth,base),at(-half,depth,base)});
    for (const auto &roof:plan.roofs) {
      double low=1e30,high=-1e30;
      for(auto p:roof.polygon){low=std::min(low,p.y);high=std::max(high,p.y);}
      if(high<=base-1.3 || low>=base+height+1.5)continue;
      const auto projected=polygon(roof.polygon);
      if(std::abs(geom2::areaMetres(projected))<1e-6)continue;
      std::vector<geom2::Polygon> overlap;
      if(!geom2::intersectionOf({geom2::toOperand(envelope),geom2::toOperand(projected)},overlap,nullptr)) {clear=false;break;}
      for(const auto &piece:overlap)if(std::abs(geom2::areaMetres(piece))>.002)clear=false;
    }
    // A full indoor landing is required at the entrance, not a stairwell.
    auto landing=at(0,-house.frame.cellMetres*.5,base);
    if (!inside(owner->boundary,landing)) clear=false;
    const auto cell=geom2::cellAtPoint(house.grid,house.frame,landing.x,landing.z);
    for(const auto &f:house.floors) if(std::abs(f.level*house.gridClass.storeyMetres-base)<.01)
      for(const auto &stair:f.stairs) for(auto c:wellCells(house.grid,stair)) if(c==cell) clear=false;
    if(!clear) continue;

    rewrite::DwellingWallTurret turret;
    turret.room=nextRoom++;turret.hostWall=host.id;turret.floor=uint32_t(level);
    turret.round=plan.wallTurrets.empty();turret.base=base;turret.top=base+height;turret.corbelBase=base-1.2;
    // D-shaped circular turrets share a flat attachment with the host wall.
    // Square turrets use the identical doorway and support contract.
    if(turret.round) {
      for(int k=0;k<=5;++k) {
        const double a=3.14159265358979323846*double(k)/5.;
        turret.footprint.push_back(at(-half*std::cos(a),depth*std::sin(a),base));
      }
    } else turret.footprint={at(-half,0,base),at(-half,depth,base),at(half,depth,base),at(half,0,base)};
    rewrite::DwellingRoom room;room.id=turret.room;room.floor=turret.floor;
    room.boundary=turret.footprint;room.ceilingHeight=turret.top;
    room.usableArea=std::max(0.,std::abs(geom2::areaMetres(polygon(room.boundary)))-.2*(2*depth+2*half));
    plan.rooms.push_back(room);
    auto slab=turret.footprint;std::reverse(slab.begin(),slab.end());
    plan.floors[level].slabs.push_back(slab);
    auto ceiling=turret.footprint;for(auto &p:ceiling)p.y=turret.top;
    plan.ceilings.push_back(ceiling);
    // Replace any window on the attachment with one walk-through opening.
    plan.openings.erase(std::remove_if(plan.openings.begin(),plan.openings.end(),[&](const auto &o){return o.wall==host.id;}),plan.openings.end());
    plan.doors.erase(std::remove_if(plan.doors.begin(),plan.doors.end(),[&](const auto &d){return d.wall==host.id;}),plan.doors.end());
    rewrite::DwellingOpening access;access.wall=host.id;access.room=hostRoom;access.floor=turret.floor;
    access.a=at(-accessWidth*.5,0,base);access.b=at(accessWidth*.5,0,base);access.top=access.head=std::min(height-.2,2.5);
    access.thickness=host.thickness;plan.openings.push_back(access);
    rewrite::DwellingDoor door;door.wall=host.id;door.room=turret.room;door.fromRoom=hostRoom;
    door.floor=turret.floor;door.center=mid;door.width=accessWidth;plan.doors.push_back(door);
    for(size_t k=0;k+1<turret.footprint.size();++k) {
      rewrite::DwellingWall wall;wall.id=nextWall++;wall.floor=turret.floor;
      wall.a=turret.footprint[k];wall.b=turret.footprint[k+1];wall.exterior=true;wall.thickness=.2;wall.height=height;
      plan.walls.push_back(wall);
      // Narrow unglazed lights fit polygon facets without stretching an arch.
      const double span=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z);
      if((!turret.round || k==1 || k==3) && span>.28) {
        rewrite::DwellingOpening o;o.wall=wall.id;o.room=room.id;o.floor=turret.floor;
        o.a={wall.a.x*.8+wall.b.x*.2,base,wall.a.z*.8+wall.b.z*.2};
        o.b={wall.a.x*.2+wall.b.x*.8,base,wall.a.z*.2+wall.b.z*.8};
        o.base=o.sill=1.;o.top=o.head=height-.4;o.kind=rewrite::OpeningKind::Window;o.exterior=true;
        plan.openings.push_back(o);
      }
    }
    const auto peak=at(0,depth*.45,turret.top+1.4);
    for(size_t k=0;k<ceiling.size();++k)
      plan.roofs.push_back({{ceiling[k],ceiling[(k+1)%ceiling.size()],peak},uint32_t(plan.storeys.size()+plan.wallTurrets.size()),false,false,turret.round});
    plan.wallTurrets.push_back(std::move(turret));
  }
}

} // namespace

bool realiseCellDwelling(const CellHouse &house,
                         const CellDwellingSettings &sourceSettings,
                         rewrite::DwellingPlan &plan,
                         std::string *error)
{
  CellDwellingSettings settings=sourceSettings;
  if(house.architecture==rewrite::DwellingArchitecture::Roman) {
    settings.roofPitch=.38;settings.roofPitchCeiling=.65;settings.roofRidgeMetres=1.6;
  }
  if(house.architecture==rewrite::DwellingArchitecture::HighFantasy){settings.roofPitch=1.2;settings.roofRidgeMetres=5.;}
  plan = rewrite::DwellingPlan();
  plan.cellMetres=house.frame.cellMetres;
  plan.siteAccess=house.siteAccess;
  const bool sacred=house.building==BuildingClass::Church || house.building==BuildingClass::Temple;
  plan.facade.enabled=true;
  plan.facade.doorProfile=settings.entranceProfile;
  if(sacred && plan.facade.doorProfile==rewrite::DoorProfile::Automatic)
    plan.facade.doorProfile=house.building==BuildingClass::Church?rewrite::DoorProfile::PointedArch:rewrite::DoorProfile::RoundArch;
  plan.facade.inwardWalls=true;
  plan.facade.roofFraming=true;
  plan.facade.corridorSupports=house.building==BuildingClass::Castle || house.building==BuildingClass::Church;
  plan.facade.style=house.building==BuildingClass::Church ? rewrite::FacadeStyle::Gothic : house.building==BuildingClass::Temple ? rewrite::FacadeStyle::Romanesque : rewrite::FacadeStyle::Domestic;
  const bool poor=house.standing==PlotStanding::Slum;
  const bool rich=house.standing==PlotStanding::Wealthy || house.standing==PlotStanding::Grand;
  // A poor house is not a black box.
  //
  // Boarding a whole wall in dark oak was the poor standing's only facade, and
  // in a quarter of poor lots that is a street of brown blocks - measured, the
  // city's boarding outweighed its plaster two to one. A poor house is
  // wattle and daub between timbers like its neighbours; boarding is what a
  // hut and the occasional shack get, and it reads as a difference again
  // precisely because it is not everywhere.
  // Deterministic per building without a seed on the house: its own frame is
  // where it stands, and two houses never stand in the same place.
  const uint64_t where = uint64_t(std::llround(std::abs(house.frame.originX) * 7.)) * 2654435761ull ^ uint64_t(std::llround(std::abs(house.frame.originZ) * 7.)) * 40503ull;
  const bool boarded = house.building==BuildingClass::Hut || (poor && (where % 4) == 0);
  plan.facade.timberFacade=house.building!=BuildingClass::Castle && !sacred && !boarded &&
    (house.standing==PlotStanding::Modest || house.standing==PlotStanding::Comfortable || poor);
  plan.facade.stoneVaults=house.building==BuildingClass::Castle || sacred;
  plan.facade.boardFacade=boarded && (house.building==BuildingClass::Dwelling || house.building==BuildingClass::Hut);
  plan.facade.castleInterior=house.building==BuildingClass::Castle;
  // A bay is a whole number of cells.
  //
  // The bay is how often a wall is glazed, and the wall is tiled on the cell,
  // so a bay that is not a whole number of cells asks for a window between
  // two tiles. Snapped, it says the same thing in the module the wall is
  // actually built in: 2.4 and 3.2 are one cell, 5.0 is two.
  plan.facade.bayMetres=poor ? 5.0 : rich ? 2.4 : 3.2;
  if(house.frame.cellMetres>.05)
    plan.facade.bayMetres=std::max(1.,std::round(plan.facade.bayMetres/house.frame.cellMetres))*house.frame.cellMetres;
  plan.facade.glazed=!poor;
  if(house.building==BuildingClass::Castle) {
    plan.facade.style=rewrite::FacadeStyle::Romanesque;
    plan.facade.glazed=false;
  }
  plan.facade.maximumWindows=settings.maximumWindows;
  if(poor)plan.facade.maximumWindows=settings.maximumWindows<0 ? 1 : std::min(1,settings.maximumWindows);

  if (!house.grid.valid())
  {
    return fail(error, "house has no grid");
  }

  if (house.floors.empty())
  {
    return fail(error, "house has no floors");
  }

  // The jetty envelope, checked rather than trusted. A brief that did not come
  // from `decideJetty` has negotiated with nobody.
  double cumulative = 0;

  for (size_t k = 1; k < house.jettyMetres.size(); ++k)
  {
    if (!(house.jettyMetres[k] >= 0) || house.jettyMetres[k] > settings.jettyPerFloorMetres)
    {
      return fail(error, "jetty projection per floor is outside the accepted envelope");
    }

    cumulative += house.jettyMetres[k];
  }

  if (cumulative > settings.jettyTotalMetres + 1e-9)
  {
    return fail(error, "cumulative jetty projection exceeds the accepted total");
  }

  const double storeyHeight = house.gridClass.storeyMetres;
  std::map<uint32_t,std::vector<WallRun>> projectedRuns;
  std::vector<std::vector<rewrite::Position>> jettySoffits;

  std::vector<std::pair<uint32_t,std::vector<rewrite::Position>>> stairFloorPatches;
  std::vector<const CellFloor *> ordered;

  if (house.basement.has_value())
  {
    ordered.push_back(&house.basement.value());
  }

  for (const CellFloor &floor : house.floors)
  {
    ordered.push_back(&floor);
  }

  const double streetLength = std::sqrt(house.streetJ * house.streetJ + house.streetI * house.streetI);

  for (const CellFloor *each : ordered)
  {
    const CellFloor &floor = *each;

    if (floor.loops.empty())
    {
      return fail(error, "floor has no boundary");
    }

    rewrite::DwellingStorey storey;
    storey.level = uint32_t(floor.level + (house.basement.has_value() ? 1 : 0));
    storey.base = double(floor.level) * storeyHeight;
    storey.top = storey.base + storeyHeight;

    std::vector<WallRun> runs = wallRuns(house.grid, floor.loops.front());

    // How far this floor stands out from the ground, which is every jetty
    // below it added up.
    double project = 0;

    for (int32_t below = 1; below <= floor.level; ++below)
    {
      if (size_t(below) < house.jettyMetres.size())
      {
        project += house.jettyMetres[size_t(below)];
      }
    }

    // The party walls first: a run whose edges the planner marked as party
    // edges moves out by the gap it recorded, so the wall stands on the lot
    // line and meets the neighbour's. Every floor, because a row house is
    // the same outline all the way up.
    if (!house.partyEdges.empty())
    {
      std::vector<double> push(runs.size(), 0.);

      for (size_t k = 0; k < runs.size(); ++k)
      {
        double gap = 0;
        size_t marked = 0;

        for (const uint32_t e : runs[k].edges)
        {
          const auto found = std::lower_bound(house.partyEdges.begin(), house.partyEdges.end(), e);

          if (found != house.partyEdges.end() && *found == e)
          {
            gap = std::max(gap, house.partyGaps[size_t(found - house.partyEdges.begin())]);
            ++marked;
          }
        }

        if (marked == runs[k].edges.size() && gap > .03)
        {
          push[k] = std::min(gap, house.frame.cellMetres) / house.frame.cellMetres;
        }
      }

      // A run moves only between two runs that stand still and turn a
      // corner with it. Pushing a run beside another that also moves, or
      // beside one running the same way, drags the shared corner two ways at
      // once and folds whatever face spans the join - a strip of boards, a
      // roof slope - into a bow tie. Those are the two cases that took a city
      // down; a run with a quiet perpendicular neighbour on each side cannot
      // fold anything.
      for (size_t k = 0; k < runs.size(); ++k)
      {
        if (push[k] <= 0)
        {
          continue;
        }

        const size_t before = (k + runs.size() - 1) % runs.size(), after = (k + 1) % runs.size();
        const bool quiet = push[before] <= 0 && push[after] <= 0 &&
                           runs[before].horizontal != runs[k].horizontal && runs[after].horizontal != runs[k].horizontal;

        if (quiet)
        {
          runs[k].project = push[k];
          ++CellHouse::partyTally().pushed;
        }
      }

      // ...but only if the ring survives it. A short run pushed out beside a
      // long one that is not can fold the outline into a bow tie - one lot in
      // three thousand, and it took the whole city down at triangulation.
      // Tried first; a fold takes the whole floor back to its lattice line.
      {
        const auto trial = runRing(house.frame, runs, storey.base);
        geom2::Polygon check;

        for (const auto &p : trial)
        {
          check.outer.push_back({geom2::toUnit(p.x), geom2::toUnit(p.z)});
        }

        if (!geom2::counterClockwise(check.outer))
        {
          geom2::reverse(check.outer);
        }

        if (trial.size() < 3 || !geom2::polygonFault(check).empty())
        {
          ++CellHouse::partyTally().folded;

          for (WallRun &run : runs)
          {
            run.project = 0;
          }
        }
      }
    }

    if (project > 0 && floor.level > 0)
    {
      const Board &under = house.floors[size_t(floor.level) - 1].area;

      for (WallRun &run : runs)
      {
        double dj = 0;
        double di = 0;
        outwardStep(run.dir, dj, di);

        // Only where the wall faces the street. A jetty is a negotiation with
        // the street - that is what its clearance was measured against - and a
        // wall over the back yard was never part of it.
        if (streetLength > 1e-9 && (dj * house.streetJ + di * house.streetI) / streetLength <= .5)
        {
          continue;
        }

        // And only where there is air outside it.
        //
        // An offset grows a plate in every direction, and on a floor that has
        // stepped back one of those directions is inward, out over the roof the
        // setback just made. A wall standing a storey above the tiles it sits
        // on is the single commonest way this geometry goes wrong, and the
        // cell model can simply refuse it: if the ground outside this wall
        // belongs to the floor below, the wall is over a roof and does not
        // move.
        bool openAir = true;

        for (const uint32_t e : run.edges)
        {
          const uint32_t outside = house.grid.neighbour(house.grid.edgeCell(e), ccw(run.dir));

          if (outside != geom2::kNoCell && house.grid.contains(under, outside))
          {
            openAir = false;
            break;
          }
        }

        if (openAir)
        {
          run.project = project / house.frame.cellMetres;
        }
      }

      storey.outer = runRing(house.frame, runs, storey.base);
    }
    else
    {
      // From the runs whenever any of them moved - a party wall pushed to
      // the lot line on a floor with no jetty was set on the run and then
      // ignored here, while the rooms and roof under it were moved by
      // `projectPoint` all the same. That is how a house's outline stayed
      // put while its boards folded.
      bool moved = false;

      for (const WallRun &run : runs)
      {
        moved = moved || run.project > 0;
      }

      storey.outer = moved ? runRing(house.frame, runs, storey.base)
                           : plainRing(house.grid, house.frame, floor.loops.front(), storey.base);
    }

    if (storey.outer.size() < 3)
    {
      return fail(error, "floor boundary has fewer than three corners");
    }

    for (size_t k = 1; k < floor.loops.size(); ++k)
    {
      storey.holes.push_back(plainRing(house.grid, house.frame, floor.loops[k], storey.base));
    }

    projectedRuns[storey.level]=runs;
    plan.storeys.push_back(std::move(storey));

    // The rooms, as the plan's own record: boundary, storey, use and kind.
    // Never filled before, and the structural step needs them - a wall is
    // two faces, one per object, and which room is on which side of it is a
    // question only the rooms can answer.
    for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
    {
      if (r >= floor.roomContours.size() || floor.roomContours[r].empty())
      {
        continue;
      }

      rewrite::DwellingRoom room;
      room.id = uint64_t(plan.rooms.size()) + 1;
      room.floor = uint32_t(floor.level + (house.basement.has_value() ? 1 : 0));
      room.use = r < floor.roomUses.size() ? floor.roomUses[r] : rewrite::RoomUse::Living;
      room.kind = r < floor.roomKinds.size() ? floor.roomKinds[r] : rewrite::RoomKind::Count;
      room.openToRoof=(r<floor.roomOutdoor.size() && floor.roomOutdoor[r]) || floor.level==house.floors.back().level &&
        (room.kind==rewrite::RoomKind::Nave || room.kind==rewrite::RoomKind::Chancel || room.kind==rewrite::RoomKind::GreatHall);
      room.boundary = plainRing(house.grid, house.frame, floor.roomContours[r], double(floor.level) * storeyHeight);
      room.usableArea = double(floor.roomAreas[r].count()) * house.frame.cellMetres * house.frame.cellMetres;
      // The lattice describes the gross envelope. Reserve the full exterior
      // thickness inside it, and half of each shared partition per room.
      room.usableArea=0;
      for(auto [i,j]:geom2::cells(floor.roomAreas[r])) {
        const auto cell=house.grid.cell(i,j);
        const auto reserve=[&](geom2::Dir direction){
          auto next=house.grid.neighbour(cell,direction);
          if(next!=geom2::kNoCell && house.grid.contains(floor.roomAreas[r],next))return 0.;
          return next!=geom2::kNoCell && house.grid.contains(floor.area,next) ? settings.innerWallMetres*.5 : settings.outerWallMetres;
        };
        room.usableArea+=std::max(0.,house.frame.cellMetres-reserve(geom2::Dir::East)-reserve(geom2::Dir::West))*
                         std::max(0.,house.frame.cellMetres-reserve(geom2::Dir::North)-reserve(geom2::Dir::South));
      }

      if (r < floor.roomHeight.size() && floor.roomHeight[r] > 0)
      {
        room.ceilingHeight = double(floor.level) * storeyHeight + floor.roomHeight[r];
      }

      plan.rooms.push_back(std::move(room));
      if(floor.level==0 && !house.coveredPassage.empty() && floor.roomAreas[r]==house.coveredPassage) {
        rewrite::DwellingCoveredPassage passage;passage.id=house.passageId;passage.room=plan.rooms.back().id;
        passage.polygon=plan.rooms.back().boundary;passage.clearance=house.passageClearance;
        for(auto [i,j]:geom2::cells(house.coveredPassage))passage.cells.push_back(house.grid.cell(i,j));
        passage.occupiedFloor=uint32_t(1+(house.basement.has_value()?1:0));plan.coveredPassages.push_back(std::move(passage));
      }
    }

    rewrite::DwellingFloor slab;
    slab.elevation = double(floor.level) * storeyHeight;
    slab.height = storeyHeight;
    slab.grossArea = double(floor.area.count()) * house.frame.cellMetres * house.frame.cellMetres;

    // The boards, one square per cell.
    //
    // Not the storey's outline as a single face. A storey of this planner is a
    // cross, an L or a block with a court in it - every one of them non-convex
    // - and a non-convex polygon handed to anything that fans it from the first
    // vertex fills its own re-entrant corners. That showed as a tan wedge of
    // floor hanging in the air outside a church, between the transept and the
    // nave, which is a place there is no building at all. A cell is a square,
    // squares are convex, and the union of them is exactly the floor.
    //
    // From the cell's own four nodes, not from its centre plus a half cell
    // each way in world x and z: on a lot turned to its street the frame is
    // rotated, and an axis-aligned square round a rotated cell's centre is
    // not that cell - the squares overlapped in the middle of a floor and
    // missed its edge, and a wall at the edge stood on nothing.
    {
      // A floor stops at the INSIDE of an exterior wall.
      //
      // A cell reaches the lattice line, and for a boundary wall the lattice
      // line is the wall's OUTER face: `centreShift` is `inward * half`, so
      // the two skins sit at the line and a whole thickness inside it. A slab
      // drawn to the line therefore runs through the entire wall and its edge
      // shows on the street, which is the floor "going into the outer wall".
      // An interior wall is centred on its line instead, so a slab reaching it
      // stops inside the wall and the two rooms' floors meet under the
      // partition - right already, and left alone.
      //
      // The inset is per SIDE, accumulated per corner, so a cell on an outside
      // corner comes in on both of its exposed sides and the mitre is exact.
      // Paving is not inset: it is outdoor ground and the wall stands on it.
      const auto square = [&](const uint32_t i, const uint32_t j, const double y, const bool indoors = true) {
        std::vector<rewrite::Position> ring;

        for (const auto &[di, dj] : std::vector<std::pair<int32_t, int32_t>>{{0, 0}, {0, 1}, {1, 1}, {1, 0}})
        {
          double x = 0, z = 0;
          geom2::nodePoint(house.grid, house.frame, house.grid.node(int32_t(i) + di, int32_t(j) + dj), x, z);
          ring.push_back(rewrite::Position{x, y, z});
        }

        if (indoors && settings.outerWallMetres > 1e-6)
        {
          const uint32_t here = house.grid.cell(i, j);
          std::array<std::pair<double, double>, 4> shift{};

          for (const Dir d : geom2::kCardinal)
          {
            const uint32_t e = house.grid.cellEdge(here, d);

            if (e == geom2::kNoEdge || !exteriorEdge(floor, e))
            {
              continue;
            }

            double ax = 0, az = 0, bx = 0, bz = 0;
            geom2::edgePoint(house.grid, house.frame, e, 0, ax, az);
            geom2::edgePoint(house.grid, house.frame, e, 1, bx, bz);

            double cx = 0, cz = 0;
            geom2::cellPoint(house.grid, house.frame, here, cx, cz);

            // Inward is from the side's midpoint toward the cell's centre.
            double nx = cx - (ax + bx) * .5, nz = cz - (az + bz) * .5;
            const double reach = std::hypot(nx, nz);

            if (reach < 1e-9)
            {
              continue;
            }

            nx = nx / reach * settings.outerWallMetres;
            nz = nz / reach * settings.outerWallMetres;

            // Matched against the ORIGINAL corners: a corner on two exposed
            // sides is found by both, and only then moved by both.
            for (size_t k = 0; k < ring.size(); ++k)
            {
              if (std::hypot(ring[k].x - ax, ring[k].z - az) < 1e-6 ||
                  std::hypot(ring[k].x - bx, ring[k].z - bz) < 1e-6)
              {
                shift[k].first += nx;
                shift[k].second += nz;
              }
            }
          }

          for (size_t k = 0; k < ring.size(); ++k)
          {
            ring[k].x += shift[k].first;
            ring[k].z += shift[k].second;
          }
        }

        return ring;
      };

      // A stair's well is the hole its flights come up through: on the floor
      // a stair ARRIVES on, the well's cells get no boards. What you step
      // onto up there is the floor beyond the well, or the arrival strip the
      // flight itself carries.
      Board wells;

      for (const CellStair &stair : floor.stairs)
      {
        if (stair.up || stair.form == StairForm::Trapdoor)
        {
          continue;
        }

        for (const uint32_t c : wellCells(house.grid, stair))
        {
          house.grid.add(wells, c);
        }
      }

      for(const auto &[i,j]:geom2::cells(floor.area & wells))
        stairFloorPatches.push_back({uint32_t(plan.floors.size()),square(i,j,slab.elevation)});

      Board outdoor;
      for(size_t r=0;r<floor.roomAreas.size();++r)
        if(floor.level==0 && r<floor.roomOutdoor.size() && floor.roomOutdoor[r])outdoor=outdoor|floor.roomAreas[r];
      // Outdoor ground has one surface owner: paving, never a coincident
      // wooden floor slab underneath it.
      for (const auto &[i, j] : geom2::cells(floor.area & ~wells & ~outdoor))
      {
        if(floor.level==0 && house.grid.contains(house.coveredPassage,house.grid.cell(i,j)))slab.paving.push_back(square(i,j,slab.elevation,false));
        else slab.slabs.push_back(square(i, j, slab.elevation));
      }

      // Ceilings belong to rooms, not to the roof renderer. Reserve upward
      // stair wells here too: a ceiling must never seal the circulation void.
      Board ceilingWells;
      for (const CellStair &stair : floor.stairs)
        if (stair.up)
          for (const uint32_t c : wellCells(house.grid, stair))
            house.grid.add(ceilingWells, c);
      for (size_t r = 0; r < floor.roomAreas.size(); ++r)
      {
        if (r < floor.roomOutdoor.size() && floor.roomOutdoor[r]) continue;
        if(floor.level==house.floors.back().level && r<floor.roomKinds.size() &&
           (floor.roomKinds[r]==rewrite::RoomKind::Nave || floor.roomKinds[r]==rewrite::RoomKind::Chancel || floor.roomKinds[r]==rewrite::RoomKind::GreatHall))continue;
        const double height = r < floor.roomHeight.size() && floor.roomHeight[r] > 0
          ? floor.roomHeight[r] : storeyHeight;
        for (const auto &[i, j] : geom2::cells(floor.roomAreas[r] & ~ceilingWells))
        {
          auto ceiling = square(i, j, slab.elevation + height);
          std::reverse(ceiling.begin(), ceiling.end());
          plan.ceilings.push_back(std::move(ceiling));
        }
      }

      if (floor.level == 0)
      {
        for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
        {
          if (r >= floor.roomOutdoor.size() || floor.roomOutdoor[r] == 0)
          {
            continue;
          }

          for (const auto &[i, j] : geom2::cells(floor.roomAreas[r] & ~wells))
          {
            slab.paving.push_back(square(i, j, slab.elevation, false));
          }
        }
      }
    }

    // Close the projected strip with a real deck and a finished underside.
    // The lattice slabs cover the original floor only.
    if(project>0 && floor.level>0) {
      // Which way a floor board winds here: taken from a slab this floor
      // already laid rather than assumed, so the deck faces the way the rest
      // of the floor does whatever the frame's handedness is.
      double slabWinding=0;
      if(!slab.slabs.empty()) {
        const auto &reference=slab.slabs.front();
        for(size_t v=0;v<reference.size();++v) {
          const auto p=reference[v],q=reference[(v+1)%reference.size()];
          slabWinding+=p.x*q.z-q.x*p.z;
        }
      }
      // The strip starts where the LATTICE SLABS now stop, not at the wall
      // line. Those are inset by the wall's thickness so a floor ends at the
      // inside of its wall, and a strip beginning at the line instead left a
      // 20 cm ring of nothing between them - the two started on opposite
      // sides of the same line. Measured, that emptied 15 of 16 jettied
      // storeys. `project` is in cells, like `line`.
      auto flushRuns=runs;
      const double flushInset=house.frame.cellMetres>1e-9 ? settings.outerWallMetres/house.frame.cellMetres : 0.;
      for(auto &r:flushRuns)r.project=-flushInset;
      const auto flush=runRing(house.frame,flushRuns,slab.elevation);
      const auto projected=runRing(house.frame,runs,slab.elevation);
      for(size_t k=0;k<runs.size();++k)if(runs[k].project>0) {
        const size_t next=(k+1)%runs.size();
        const auto a=flush[k],b=flush[next],c=projected[next],d=projected[k];
        auto underside=std::vector<rewrite::Position>{d,c,b,a};
        for(auto &p:underside)p.y-=.12;
        jettySoffits.push_back(underside);
        // Close the exposed edge between the deck and its underside. Joist
        // ends alone leave a continuous daylight slot beneath the wall.
        auto rimA=d,rimB=c;rimA.y= rimB.y=slab.elevation-.06;
        plan.timbers.push_back({uint32_t(floor.level+(house.basement.has_value()?1:0)),
            rewrite::TimberKind::Beam,rimA,rimB,.16,.12});
        // ...and a deck on top of it. The soffit alone left the projected
        // strip as a gap between the storey's walls and the floor it stands
        // on: from inside the room the ground simply stopped short of the
        // jettied wall. The strip's boards wind the way the lattice slabs do.
        {
          std::vector<rewrite::Position> deck{a,b,c,d};
          double turn=0;
          for(size_t v=0;v<deck.size();++v) {
            const auto p=deck[v],q=deck[(v+1)%deck.size()];
            turn+=p.x*q.z-q.x*p.z;
          }
          if((turn<0)!=(slabWinding<0))std::reverse(deck.begin(),deck.end());
          slab.slabs.push_back(std::move(deck));
        }
        const double length=std::hypot(b.x-a.x,b.z-a.z);
        const unsigned bays=std::max(1u,unsigned(std::ceil(length/1.4)));
        const double increment=size_t(floor.level)<house.jettyMetres.size()?house.jettyMetres[floor.level]:0;
        for(unsigned j=0;j<bays;++j) {
          const double t=(j+.5)/bays;
          rewrite::Position inner{a.x+(b.x-a.x)*t,slab.elevation-.20,a.z+(b.z-a.z)*t};
          rewrite::Position outer{d.x+(c.x-d.x)*t,slab.elevation-.20,d.z+(c.z-d.z)*t};
          plan.timbers.push_back({uint32_t(floor.level+(house.basement.has_value()?1:0)),rewrite::TimberKind::Beam,inner,outer,.16,.20});
          if(increment>.05) {
            const double fraction=std::min(1.,increment/project);
            auto foot=outer;foot.x+=(inner.x-outer.x)*fraction;foot.z+=(inner.z-outer.z)*fraction;foot.y-=.35;
            auto head=outer;head.x+=(inner.x-outer.x)*.08;head.z+=(inner.z-outer.z)*.08;
            plan.timbers.push_back({uint32_t(floor.level+(house.basement.has_value()?1:0)),rewrite::TimberKind::Brace,foot,head,.12,.14});
          }
        }
      }
    }
    plan.floors.push_back(std::move(slab));
  }

  // ---- the walls, and what is cut in them -----------------------------------
  //
  // One wall per lattice edge, which is more walls than a merged run would give
  // and is the right trade here: an edge is exactly the unit a door, a window
  // and a room boundary are all expressed in, so nothing has to be split back
  // apart later. An OPEN door gets no wall at all - it is a full cell-width
  // archway, and that is the difference between it and a doorway.
  {
    uint64_t wallId = 1;

    // Which room a contour edge belongs to, and how tall that room stands.
    const auto roomOf = [&](const uint32_t e) {
      return house.grid.edgeCell(e);
    };

    const auto heightOf = [&](const CellFloor &floor, const uint32_t cell) {
      if (cell == geom2::kNoCell)
      {
        return 0.;
      }

      for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
      {
        if (house.grid.contains(floor.roomAreas[r], cell))
        {
          const double own = r < floor.roomHeight.size() ? floor.roomHeight[r] : 0.;

          return own > 0 ? own : storeyHeight;
        }
      }

      return 0.;
    };

    for (const CellFloor *each : ordered)
    {
      const CellFloor &floor = *each;
      const double base = double(floor.level) * storeyHeight;
      const uint32_t storeyIndex = uint32_t(floor.level + (house.basement.has_value() ? 1 : 0));

      // Which edges carry what.
      std::map<uint32_t, const CellDoor *> doorAt;

      for (const CellDoor &door : floor.doors)
      {
        doorAt[door.edge] = &door;
        doorAt[house.grid.twin(door.edge)] = &door;
      }

      std::map<uint32_t, uint8_t> windowAt;

      for (const uint32_t e : floor.windows)
      {
        windowAt[e] = 1;
      }

      std::map<uint32_t, uint8_t> done;

      // Per pen room, the lane-most fence panel: it becomes the gate.
      struct Gate
      {
        uint64_t wall = 0;
        double along = -1e300;
        uint32_t edge = geom2::kNoEdge;
        double thickness = .1, height = 1.1;
      };

      std::map<uint32_t, Gate> penGates;

      for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
      {
        for (const uint32_t e : floor.roomContours[r])
        {
          const uint32_t twin = house.grid.twin(e);

          if (done.count(e) != 0 || done.count(twin) != 0)
          {
            continue;
          }

          done[e] = 1;

          const bool exterior = exteriorEdge(floor, e);

          // A yard's own walls.
          //
          // The edges it shares with the house are the house's walls and are
          // built from the house's side; the edges with nothing beyond them
          // are a yard wall - waist high, a fence in stone - and not a storey
          // of blank wall round an open square. Built to the storey, a yard
          // read as a room somebody had forgotten to roof.
          const bool outdoor = r < floor.roomOutdoor.size() && floor.roomOutdoor[r] != 0;

          if (outdoor)
          {
            const uint32_t beyond = twin == geom2::kNoEdge ? geom2::kNoCell : house.grid.edgeCell(twin);
            const bool againstTheHouse = beyond != geom2::kNoCell && house.grid.contains(floor.area, beyond);
            const bool againstAnotherCourt =
              beyond != geom2::kNoCell && !againstTheHouse && house.grid.contains(house.courts, beyond);

            // ...nor where a shed stands beyond it: the shed's wall is the
            // fence there, and its door opens onto the pen through it.
            const bool againstAShed = beyond != geom2::kNoCell &&
                                      std::find(house.sheds.begin(), house.sheds.end(), beyond) != house.sheds.end();

            if (againstTheHouse || againstAnotherCourt || againstAShed)
            {
              // Left for the other side to build; undone so that side is not
              // skipped as already done.
              done.erase(e);
              continue;
            }

            // An outdoor room ABOVE the ground is a terrace, and it gets a
            // balustrade - it used to get nothing at all.
            //
            // This `continue` and the floor boards disagree about which
            // storey the rule applies to: the boards exclude an outdoor room
            // only on level 0, so a terrace on a first floor is laid with
            // boards, while the walls skipped every level but 0 and built
            // nothing round them - a floor slab standing in the open with no
            // edge to it. LATENT: no plan in the corpus puts an outdoor room
            // above the ground, so closing it changed no measurement. It is
            // closed because the two halves of one rule must agree, not
            // because it was the cause of anything seen.
            //
            // A terrace is not a yard, so it is not a stone wall round an
            // open square: it is the edge of a floor that stops before the
            // room does, which is exactly `WallKind::Rail`.
            const bool terrace = floor.level != 0;

            double ax = 0, az = 0, bx = 0, bz = 0;
            geom2::edgePoint(house.grid, house.frame, e, 0, ax, az);
            geom2::edgePoint(house.grid, house.frame, e, 1, bx, bz);

            // A pen's fence is boards, low, and one of its lane-side panels
            // is the gate; a yard's is a stone wall.
            const bool pen = !house.pens.empty() && (floor.roomAreas[r] & ~house.pens).empty();

            rewrite::DwellingWall fence;
            fence.id = wallId++;
            fence.floor = storeyIndex;
            fence.a = rewrite::Position{ax, base, az};
            fence.b = rewrite::Position{bx, base, bz};
            fence.thickness = terrace ? .08 : (pen ? .1 : settings.outerWallMetres * .6);
            fence.exterior = true;
            fence.kind = terrace ? rewrite::WallKind::Rail : rewrite::WallKind::Solid;
            fence.height = terrace ? settings.balconyRailHeightMetres
                                   : (pen ? settings.penFenceMetres : settings.yardWallMetres);
            plan.walls.push_back(fence);

            if (pen && !terrace)
            {
              const uint32_t c = house.grid.edgeCell(e);
              const double along = double(house.grid.cellColumn(c)) * house.streetJ + double(house.grid.cellRow(c)) * house.streetI;
              auto &gate = penGates[r];

              if (gate.wall == 0 || along > gate.along)
              {
                gate.wall = fence.id;
                gate.along = along;
                gate.edge = e;
                gate.thickness = fence.thickness;
                gate.height = fence.height;
              }
            }

            continue;
          }

          const auto found = doorAt.find(e);
          const CellDoor *door = found == doorAt.end() ? nullptr : found->second;

          // An open door is an ARCH, not a missing wall.
          //
          // It was a missing wall, and that is the thing the reference plans
          // never show: two rooms with no division between them are one room.
          // An archway has jambs and a head - you can see the wall it is cut
          // into, and the storey above stands on the spandrel - so the wall is
          // built and the opening is cut wide and tall instead.
          const bool arch = door != nullptr && door->kind == DoorKind::Open;

          double ax = 0, az = 0, bx = 0, bz = 0;
          geom2::edgePoint(house.grid, house.frame, e, 0, ax, az);
          geom2::edgePoint(house.grid, house.frame, e, 1, bx, bz);

          rewrite::DwellingWall wall;
          wall.id = wallId++;
          wall.floor = storeyIndex;
          wall.a = rewrite::Position{ax, base, az};
          wall.b = rewrite::Position{bx, base, bz};
          wall.thickness = exterior ? settings.outerWallMetres : settings.innerWallMetres;
          wall.exterior = exterior;
          wall.party = exterior && (std::binary_search(house.partyEdges.begin(),house.partyEdges.end(),e) ||
                                    std::binary_search(house.partyEdges.begin(),house.partyEdges.end(),twin));
          wall.kind = rewrite::WallKind::Solid;
          // As tall as the tallest room it divides.
          //
          // Which is the whole rule, and it gives the right answer at every
          // junction without a case for any of them: the wall round a nave is
          // the nave's height, the wall between a nave and a transept is the
          // nave's height too - and the transept's roof then dies into it as a
          // gable, because a roof dies into anything taller than itself.
          wall.height = std::max(heightOf(floor, roomOf(e)), heightOf(floor, roomOf(twin)));
          plan.walls.push_back(wall);

          const uint64_t id = wall.id;
          if(floor.level==0 && std::any_of(house.passagePortals.begin(),house.passagePortals.end(),[&](auto p){return p==e || p==twin;})) {
            plan.facade.passageWalls.push_back(id);
            if(!plan.coveredPassages.empty()) {
              auto &passage=plan.coveredPassages.front();passage.portalWalls.push_back(id);
              rewrite::DwellingDoor portal;portal.wall=id;portal.room=passage.room;portal.floor=storeyIndex;
              portal.center={(ax+bx)*.5,base,(az+bz)*.5};portal.width=house.frame.cellMetres;portal.exterior=true;
              plan.doors.push_back(portal);
            }
            continue;
          }

          // The front door, which until now was chosen and never built.
          //
          // `house.entrance` decided where the door went, every rule downstream
          // respected it - no stair on its landing, no furniture in it, windows
          // routed around it - and nothing ever cut the hole. The realised
          // building had no way in. It is a door like any other except that its
          // far side is the street, so it has no second room.
          const bool front = floor.level == 0 && (e == house.entrance || twin == house.entrance);

          if (door != nullptr || front)
          {
            // A pair of doors shares its jambs, so it is not twice as wide; and
            // a threshold this size is cut into one cell, which is what the
            // clamp is for.
            const uint8_t leaves = front ? (settings.entranceLeaves?std::clamp<uint8_t>(settings.entranceLeaves,1,2):(sacred || settings.formalFront?2u:1u)) : door != nullptr ? door->leaves : 1u;

            // An arch is as wide as the wall it is cut into will allow; a door
            // is as wide as its leaves.
            const double width =
              arch ? std::min(house.frame.cellMetres-.10,std::max(settings.doorWidthMetres,house.frame.cellMetres * (leaves >= 2 ? .82 : .66)))
                   : std::min(house.frame.cellMetres - .10,
                              front ? std::max(settings.entranceWidthMetres,sacred?2.6:settings.doorWidthMetres * (leaves>=2?settings.doubleDoorScale:1.)) : settings.doorWidthMetres * (leaves >= 2 ? settings.doubleDoorScale : 1.));

            // The frame occupies part of the opening: leave at least two
            // metres of clear interior headroom below its lintel.
            const double head = front ? std::min(wall.height-.3,std::max(settings.entranceHeightMetres,sacred?3.6:2.3)) : arch ? std::min(wall.height - .5, 2.55) : std::min(storeyHeight-.25,2.3);

            double mx = 0, mz = 0;
            geom2::edgePoint(house.grid, house.frame, e, .5, mx, mz);

            rewrite::DwellingDoor leaf;
            leaf.wall = id;
            const auto roomId = [&](const uint32_t local) {
              uint32_t at = 0;

              for (const rewrite::DwellingRoom &room : plan.rooms)
              {
                if (room.floor == storeyIndex && at++ == local)
                {
                  return room.id;
                }
              }

              return uint64_t(0);
            };

            leaf.room = roomId(door != nullptr ? door->a : r);
            leaf.fromRoom = door != nullptr ? roomId(door->b) : 0;
            leaf.floor = storeyIndex;
            leaf.center = rewrite::Position{mx, base, mz};
            leaf.width = width;
            leaf.leaves = leaves;
            leaf.privateAccess = front || (door != nullptr && door->kind == DoorKind::Solid);
            leaf.exterior = exterior || front;
            plan.doors.push_back(leaf);

            rewrite::DwellingOpening cut;
            cut.wall = id;
            cut.room = door != nullptr ? door->a : 0;
            cut.floor = storeyIndex;
            const double half = width * .5 / house.frame.cellMetres;
            geom2::edgePoint(house.grid, house.frame, e, .5 - half, cut.a.x, cut.a.z);
            geom2::edgePoint(house.grid, house.frame, e, .5 + half, cut.b.x, cut.b.z);
            cut.a.y = base;
            cut.b.y = base;
            cut.base = 0;
            cut.top = head;
            cut.sill = 0;
            cut.head = head;
            cut.thickness = wall.thickness;
            cut.kind = rewrite::OpeningKind::Door;
            cut.exterior = exterior || front;
            plan.openings.push_back(cut);
          }

          if (windowAt.count(e) != 0 || windowAt.count(twin) != 0)
          {
            rewrite::DwellingOpening glass;
            glass.wall = id;
            glass.room = r;
            glass.floor = storeyIndex;
            geom2::edgePoint(house.grid, house.frame, e, .2, glass.a.x, glass.a.z);
            geom2::edgePoint(house.grid, house.frame, e, .8, glass.b.x, glass.b.z);
            glass.a.y = base;
            glass.b.y = base;
            glass.base = settings.windowSillMetres;
            glass.top = settings.windowSillMetres + settings.windowHeightMetres;
            glass.sill = settings.windowSillMetres;
            glass.head = glass.top;
            glass.thickness = wall.thickness;
            glass.kind = rewrite::OpeningKind::Window;
            glass.exterior = true;
            plan.openings.push_back(glass);
          }
        }
      }

      // The gates: one opening cut through each pen's lane-most fence panel,
      // the whole fence tall, a metre wide, no leaf - a gap you drive the
      // beasts through. The house's own door onto the pen is the doors'
      // business and comes with every yard.
      for (const auto &[room, gate] : penGates)
      {
        if (gate.wall == 0 || gate.edge == geom2::kNoEdge)
        {
          continue;
        }

        const double width = std::min(house.frame.cellMetres * .6, 1.2);
        const double half = width * .5 / house.frame.cellMetres;
        rewrite::DwellingOpening cut;
        cut.wall = gate.wall;
        cut.room = room;
        cut.floor = storeyIndex;
        geom2::edgePoint(house.grid, house.frame, gate.edge, .5 - half, cut.a.x, cut.a.z);
        geom2::edgePoint(house.grid, house.frame, gate.edge, .5 + half, cut.b.x, cut.b.z);
        cut.a.y = base;
        cut.b.y = base;
        cut.base = 0;
        cut.top = gate.height;
        cut.sill = 0;
        cut.head = gate.height;
        cut.thickness = gate.thickness;
        cut.kind = rewrite::OpeningKind::Door;
        cut.exterior = true;
        plan.openings.push_back(cut);
      }
    }

    // ---- the sheds (P27) -----------------------------------------------------
    //
    // One cell each, standing on the lot beside a pen: four walls to a shed's
    // height, a floor, a door onto the pen, and a hipped roof of its own with
    // an overhang. Not a room of the house - nothing joins it but the pen.
    for (const uint32_t shed : house.sheds)
    {
      const double height = settings.shedMetres;
      const double thickness = std::max(.15, settings.outerWallMetres * .5);
      const uint32_t row = house.grid.cellRow(shed), column = house.grid.cellColumn(shed);
      std::vector<rewrite::Position> corners;

      for (const auto &[di, dj] : std::vector<std::pair<int32_t, int32_t>>{{0, 0}, {0, 1}, {1, 1}, {1, 0}})
      {
        double x = 0, z = 0;
        geom2::nodePoint(house.grid, house.frame, house.grid.node(int32_t(row) + di, int32_t(column) + dj), x, z);
        corners.push_back(rewrite::Position{x, 0, z});
      }

      // Which side faces the pen: the door goes there.
      Dir toPen = Dir::North;
      bool found = false;

      for (const Dir d : geom2::kCardinal)
      {
        const uint32_t n = house.grid.neighbour(shed, d);

        if (n != geom2::kNoCell && house.grid.contains(house.pens, n))
        {
          toPen = d;
          found = true;
          break;
        }
      }

      for (const Dir d : geom2::kCardinal)
      {
        const uint32_t e = house.grid.cellEdge(shed, d);

        if (e == geom2::kNoEdge)
        {
          continue;
        }

        double ax = 0, az = 0, bx = 0, bz = 0;
        geom2::edgePoint(house.grid, house.frame, e, 0, ax, az);
        geom2::edgePoint(house.grid, house.frame, e, 1, bx, bz);

        rewrite::DwellingWall wall;
        wall.id = wallId++;
        wall.floor = uint32_t(house.basement.has_value() ? 1 : 0);
        wall.a = rewrite::Position{ax, 0, az};
        wall.b = rewrite::Position{bx, 0, bz};
        wall.thickness = thickness;
        wall.exterior = true;
        wall.kind = rewrite::WallKind::Solid;
        wall.height = height;
        plan.walls.push_back(wall);

        if (found && d == toPen)
        {
          const double width = std::min(house.frame.cellMetres * .5, .9);
          const double half = width * .5 / house.frame.cellMetres;
          rewrite::DwellingOpening cut;
          cut.wall = wall.id;
          cut.room = 0;
          cut.floor = wall.floor;
          geom2::edgePoint(house.grid, house.frame, e, .5 - half, cut.a.x, cut.a.z);
          geom2::edgePoint(house.grid, house.frame, e, .5 + half, cut.b.x, cut.b.z);
          cut.a.y = 0;
          cut.b.y = 0;
          cut.base = 0;
          cut.top = std::min(height - .3, 1.95);
          cut.sill = 0;
          cut.head = cut.top;
          cut.thickness = thickness;
          cut.kind = rewrite::OpeningKind::Door;
          cut.exterior = true;
          plan.openings.push_back(cut);
        }
      }

      // The floor: the cell's square on the ground storey's slab.
      for (rewrite::DwellingFloor &level : plan.floors)
      {
        if (std::abs(level.elevation) < 1e-6)
        {
          level.slabs.push_back(corners);
          break;
        }
      }

    }
  }

  // ---- the flights ----------------------------------------------------------
  //
  // From the well the planner sized (P29). The well is a rectangle of cells
  // from the entry cell, `length` in `dir` and `across` to the clockwise side
  // of it; its frame is `along` (down the well from the
  // entry edge) and `across` (to the right of that). A straight flight runs
  // from the entry edge up the well and arrives on a strip of floor at the
  // far end; a half turn runs up one side of the well to a landing at the far
  // end and back down the other side, arriving at the entry edge. The landing
  // is a piece of floor half way up that both flights connect to. Every
  // measure - risers, tread, width, landing - is the planner's; nothing is
  // re-derived here.
  for (const CellFloor *each : ordered)
  {
    const CellFloor &floor = *each;

    for (const CellStair &stair : floor.stairs)
    {
      if (!stair.up || stair.form == StairForm::Trapdoor || stair.risers == 0)
      {
        continue;
      }

      double cx = 0, cz = 0;
      geom2::cellPoint(house.grid, house.frame, stair.cell, cx, cz);

      const auto world = [&](const geom2::Dir d, double &x, double &z) {
        double ox = 0, oz = 0;
        geom2::framePoint(house.frame, 0, 0, ox, oz);
        geom2::framePoint(house.frame, double(geom2::deltaJ(d)), double(geom2::deltaI(d)), x, z);
        x -= ox;
        z -= oz;
        const double length = std::sqrt(x * x + z * z);

        if (length > 1e-9)
        {
          x /= length;
          z /= length;
        }
      };

      double alongX = 0, alongZ = 0, acrossX = 0, acrossZ = 0;
      world(stair.dir, alongX, alongZ);
      world(cw(stair.dir), acrossX, acrossZ);

      const double cell = house.frame.cellMetres;
      const double margin = wellMargin(settings);

      // The well is a RECTANGLE: `length` cells along it and `across` cells
      // wide. `wellCells` returns every cell in it, so its size is the
      // product of the two, and measuring the well's length by that count
      // made the well `across` times too long: the landing at the top of the
      // first flight was drawn from the head of the flight to a far end that
      // did not exist, and on a two-cell-wide well it ran five metres out of
      // the house and hung over the street. That plank is what reads as a
      // brown projection from the roofline. The width was the mirror of the
      // same mistake - one cell, whatever `across` said - which left a half
      // turn's two flights no room between them and stood them on each other.
      const double along = double(std::max(1u, stair.length));
      const double wide = double(std::max(1u, stair.across));
      const double wellLength = along * cell - 2. * margin; // Inside the walls.
      const double half = wide * cell * .5 - margin;        // Half the well's inside width.
      const double base = double(floor.level) * storeyHeight;
      const double top = base + storeyHeight; // Finished floor and stair arrival share one elevation.
      const uint32_t storeyIndex = uint32_t(floor.level + (house.basement.has_value() ? 1 : 0));

      // A point in the well's frame: `a` down the well from the entry edge,
      // `b` across it from its centre line.
      const auto at = [&](const double a, const double b, const double y) {
        // `b` is measured from the well's centre line, and the well grows to
        // the clockwise side of the entry CELL: on a well more than one cell
        // wide its centre is half of the extra width over from that cell's
        // own centre.
        const double centre = (wide - 1.) * cell * .5;
        const double ex = cx - alongX * (cell * .5 - margin) + acrossX * centre;
        const double ez = cz - alongZ * (cell * .5 - margin) + acrossZ * centre;

        return rewrite::Position{ex + alongX * a + acrossX * b, y, ez + alongZ * a + acrossZ * b};
      };

      const auto strip = [&](const double a0, const double a1, const double b0, const double b1, const double y) {
        return std::vector<rewrite::Position>{at(a0, b0, y), at(a0, b1, y), at(a1, b1, y), at(a1, b0, y)};
      };

      const double going = stair.tread * double(std::max(1u, stair.risers) - 1u);
      const double width = stair.width;
      const double legs = stair.form == StairForm::HalfTurn ? 2. : 1.;
      const double climb = (top - base) / legs;

      if (stair.form == StairForm::Straight)
      {
        // One flight up the middle of the well, and the rest of the well's
        // far end is the strip you arrive on, the well's full width so there
        // is nothing to fall past on either side of the flight.
        rewrite::DwellingStair flight;
        flight.opening = strip(0, going, -width * .5, width * .5, base);
        flight.opening[2].y = flight.opening[3].y = base + climb;
        // Carry the arrival across the wall clearance to the next floor cell.
        flight.upperLanding = strip(going, wellLength + margin, -half, half, base + climb);
        flight.lowerFloor = storeyIndex;
        flight.rise = climb;
        flight.risers = stair.risers;
        flight.tread = stair.tread;
        flight.width = width;
        flight.startRise = 0;
        flight.privateAccess = false;
        plan.stairs.push_back(std::move(flight));
      }
      else
      {
        // Two flights side by side: up the right of the well to the landing,
        // back down the left of it to the entry edge. The landing spans the
        // well's width at the far end and is the floor between them.
        // Mirrored, the climbing flight is on the left and the arrival on the
        // right; `side` flips the across axis and everything else reads the
        // same.
        const double side = stair.mirrored ? -1. : 1.;
        const double gap = std::max(0., 2. * half - 2. * width) * .5;
        const double rightInner = side * gap * .5, leftInner = -side * gap * .5;
        const double landingFrom = going;

        rewrite::DwellingStair first;
        first.opening = strip(0, going, std::min(rightInner, rightInner + side * width), std::max(rightInner, rightInner + side * width), base);
        first.opening[2].y = first.opening[3].y = base + climb;
        first.upperLanding = strip(landingFrom, wellLength, -half, half, base + climb);
        first.lowerFloor = storeyIndex;
        first.rise = climb;
        first.risers = stair.risers;
        first.tread = stair.tread;
        first.width = width;
        first.startRise = 0;
        first.privateAccess = false;
        plan.stairs.push_back(std::move(first));

        // The second runs the other way: its foot at the landing, its head at
        // the entry edge, on the left of the well.
        rewrite::DwellingStair second;
        second.opening = {at(going, leftInner, base + climb), at(going, leftInner - side * width, base + climb),
                          at(0, leftInner - side * width, base + 2. * climb), at(0, leftInner, base + 2. * climb)};
        // The return flight ends inside the well by `margin`. The floor
        // above excludes the entire well cell, so bridge that clearance to
        // the entry edge; otherwise its last tread ends at an open slot.
        second.upperLanding = strip(-margin, 0, -half - margin, half + margin, base + 2. * climb);
        second.lowerFloor = storeyIndex;
        second.rise = climb;
        second.risers = stair.risers;
        second.tread = stair.tread;
        second.width = width;
        second.startRise = climb;
        second.privateAccess = false;
        plan.stairs.push_back(std::move(second));
      }
    }
  }

  // Reserve cells for planning, but cut only the constructed stair footprint
  // from the finished floor. Reclaim unused margins without covering a flight
  // or emitting a second surface over its arrival landing.
  for(const auto &[level,patch]:stairFloorPatches) {
    if(level>=plan.floors.size() || level==0)continue;
    const auto polygon=[](const auto &ring){geom2::Polygon p;for(auto q:ring)p.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});
      if(!geom2::counterClockwise(p.outer))geom2::reverse(p.outer);return p;};
    std::vector<geom2::Operand> cuts;
    for(const auto &flight:plan.stairs)if(!flight.external && flight.lowerFloor+1==level)
      for(const auto *ring:{&flight.opening,&flight.lowerLanding,&flight.upperLanding})
        if(ring->size()>=3)cuts.push_back(geom2::toOperand(polygon(*ring)));
    if(cuts.empty())continue;
    std::vector<geom2::Polygon> remainder;std::string why;
    if(!geom2::differenceOf(geom2::toOperand(polygon(patch)),cuts,remainder,&why)) {
      if(error)*error="stair floor cutout: "+why;return false;
    }
    for(const auto &part:remainder) {
      std::vector<geom2::Point> vertices;std::vector<std::array<uint32_t,3>> triangles;
      if(!geom3::triangulate(part,triangles,vertices,&why)){if(error)*error="stair floor patch: "+why;return false;}
      for(auto tri:triangles){std::vector<rewrite::Position> face;
        for(auto k:tri)face.push_back({geom2::toMetres(vertices[k].x),plan.floors[level].elevation,geom2::toMetres(vertices[k].y)});
        plan.floors[level].slabs.push_back(std::move(face));}
    }
  }

  // ---- the roofs -----------------------------------------------------------
  {
    Stream stream(house.floors.size() * 2654435761u + 12345u);
    CellRoofs roofs;
    Ledger ledger;

    if (planRoofs(house, settings, stream, roofs, ledger, nullptr))
    {
      plan.roofs = std::move(roofs.faces);

      // The sheds' roofs (P27), after the house's so they are not thrown away
      // with the list: a hip to a point over each shed cell.
      for (const uint32_t shed : house.sheds)
      {
        const double height = settings.shedMetres;
        const uint32_t row = house.grid.cellRow(shed), column = house.grid.cellColumn(shed);
        std::vector<rewrite::Position> corners;

        for (const auto &[di, dj] : std::vector<std::pair<int32_t, int32_t>>{{0, 0}, {0, 1}, {1, 1}, {1, 0}})
        {
          double x = 0, z = 0;
          geom2::nodePoint(house.grid, house.frame, house.grid.node(int32_t(row) + di, int32_t(column) + dj), x, z);
          corners.push_back(rewrite::Position{x, 0, z});
        }

        // The roof: a hip to a point, overhanging the walls, and the soffit
        // under the overhang is left to the eaves rule of the structure.
        const double overhang = settings.eavesOverhangMetres > 0 ? settings.eavesOverhangMetres : .3;
        double cx = 0, cz = 0;

        for (const rewrite::Position &q : corners)
        {
          cx += q.x * .25;
          cz += q.z * .25;
        }

        std::vector<rewrite::Position> eaves;

        for (const rewrite::Position &q : corners)
        {
          const double dx = q.x - cx, dz = q.z - cz;
          const double len = std::hypot(dx, dz);
          eaves.push_back(rewrite::Position{q.x + dx / len * overhang * 1.4142, height, q.z + dz / len * overhang * 1.4142});
        }

        const rewrite::Position apex{cx, height + house.frame.cellMetres * .5, cz};

        for (size_t k = 0; k < 4; ++k)
        {
          rewrite::DwellingRoofFace face;
          face.wing = uint32_t(house.floors.size()) + 1u + uint32_t(&shed - house.sheds.data());
          face.gable = false;
          face.polygon = {eaves[k], eaves[(k + 1) % 4], apex};
          plan.roofs.push_back(std::move(face));
        }
      }

      for (rewrite::DwellingPillar &stack : roofs.chimneys)
      {
        plan.chimneys.push_back(std::move(stack));
      }

      // The parapet is a wall, and it goes where the walls go. Numbered after
      // them so a fortified building's wall ids stay contiguous with the rest.
      uint64_t nextWallId = 1;

      for (const rewrite::DwellingWall &wall : plan.walls)
      {
        nextWallId = std::max(nextWallId, wall.id + 1);
      }

      for (rewrite::DwellingWall &wall : roofs.parapets)
      {
        wall.id = nextWallId++;
        plan.walls.push_back(std::move(wall));
      }

      // Dormers are planned from the finished roof, not inferred by the mesh.
      // They are apertures, not boxes laid on top of an intact roof: the
      // carrying triangle is partitioned around the opening, the cheeks meet
      // that boundary, and the little roof runs back into the main slope.
      const uint64_t dormerRoll = (plan.seed * 11400714819323198485ull + 0xD04E3ull) >> 11;
      const double dormerUnit = double(dormerRoll & ((1ull << 53) - 1)) / double(1ull << 53);

      if (!poor && settings.maximumDormers > 0 && settings.dormerChance > 0 && dormerUnit < settings.dormerChance)
      {
        std::vector<rewrite::DwellingRoofFace> sourceRoofs = std::move(plan.roofs);
        plan.roofs.clear();
        plan.roofs.reserve(sourceRoofs.size() + settings.maximumDormers * 2u);

        for (size_t sourceRoofIndex = 0; sourceRoofIndex < sourceRoofs.size(); ++sourceRoofIndex)
        {
          const rewrite::DwellingRoofFace &roof = sourceRoofs[sourceRoofIndex];
          if (plan.dormers.size() >= settings.maximumDormers || roof.gable || roof.soffit || roof.polygon.size() != 3)
          {
            plan.roofs.push_back(roof);
            continue;
          }

          std::array<uint32_t, 3> order{0, 1, 2};
          std::sort(order.begin(), order.end(), [&](const uint32_t a, const uint32_t b) {
            return roof.polygon[a].y < roof.polygon[b].y;
          });

          const rewrite::Position &e0 = roof.polygon[order[0]];
          const rewrite::Position &e1 = roof.polygon[order[1]];
          const rewrite::Position &peak = roof.polygon[order[2]];
          const double span = std::hypot(e1.x - e0.x, e1.z - e0.z);
          const double run = std::hypot(peak.x - (e0.x + e1.x) * .5, peak.z - (e0.z + e1.z) * .5);
          const double rise = peak.y - (e0.y + e1.y) * .5;

          if (span < std::max(2.5, settings.dormerWidthMetres * 1.7) ||
              run < settings.dormerDepthMetres * 1.35 || rise < .55)
          {
            plan.roofs.push_back(roof);
            continue;
          }

          const double widthFraction = std::min(.72, settings.dormerWidthMetres / span);
          double segmentCentre = .5;
          uint64_t facadeWall = 0;

          // Keep the dormer in the facade's rhythm. Prefer the centre of an
          // exterior window on the eaves wall below; both are runs on the same
          // rectilinear wall segment. The small overhang means their lines are
          // near rather than coincident, hence the distance tolerance.
          double bestOpening = 1e300;
          double ex = (e1.x - e0.x) / span, ez = (e1.z - e0.z) / span;
          for (const rewrite::DwellingOpening &opening : plan.openings)
          {
            if (!opening.exterior || opening.kind != rewrite::OpeningKind::Window) continue;
            const double ox = opening.b.x - opening.a.x, oz = opening.b.z - opening.a.z;
            const double ol = std::hypot(ox, oz);
            if (ol < 1e-6 || std::abs((ox * ez - oz * ex) / ol) > .08) continue;
            const double mx = (opening.a.x + opening.b.x) * .5, mz = (opening.a.z + opening.b.z) * .5;
            const double along = ((mx - e0.x) * ex + (mz - e0.z) * ez) / span;
            const double perpendicular = std::abs((mx - e0.x) * ez - (mz - e0.z) * ex);
            if (along <= widthFraction * .6 || along >= 1. - widthFraction * .6 || perpendicular > .75) continue;
            const double score = perpendicular + std::abs(along - .5) * .05;
            if (score < bestOpening) { bestOpening = score; segmentCentre = along; facadeWall = opening.wall; }
          }

          // A blank facade still has real structural segments. Snap to the
          // nearest parallel one so every dormer names the wall rhythm it
          // continues, even when there is no window directly below it.
          if (facadeWall == 0)
          {
            for (const rewrite::DwellingWall &wall : plan.walls)
            {
              if (!wall.exterior) continue;
              const double wx = wall.b.x - wall.a.x, wz = wall.b.z - wall.a.z;
              const double wl = std::hypot(wx, wz);
              if (wl < .3) continue;
              const double mx = (wall.a.x + wall.b.x) * .5, mz = (wall.a.z + wall.b.z) * .5;
              const double along = ((mx - e0.x) * ex + (mz - e0.z) * ez) / span;
              const double perpendicular = std::abs((mx - e0.x) * ez - (mz - e0.z) * ex);
              if (along <= widthFraction * .6 || along >= 1. - widthFraction * .6) continue;
              const double angular = std::abs((wx * ez - wz * ex) / wl);
              const double score = angular * 100. + perpendicular + std::abs(along - .5) * .05;
              if (score < bestOpening) { bestOpening = score; segmentCentre = along; facadeWall = wall.id; }
            }
          }

          if (facadeWall == 0)
          {
            for (const rewrite::DwellingWall &wall : plan.walls)
            {
              if (!wall.exterior) continue;
              const double wx = wall.b.x - wall.a.x, wz = wall.b.z - wall.a.z;
              const double wl = std::hypot(wx, wz);
              if (wl < .3) continue;
              const double mx = (wall.a.x + wall.b.x) * .5, mz = (wall.a.z + wall.b.z) * .5;
              const double perpendicular = std::abs((mx - e0.x) * ez - (mz - e0.z) * ex);
              const double angular = std::abs((wx * ez - wz * ex) / wl);
              const double score = angular * 100. + perpendicular;
              if (score < bestOpening) { bestOpening = score; facadeWall = wall.id; }
            }
          }

          // Use the named facade segment as the exact basis. The roof eave is
          // geometrically parallel, but clipping/offset arithmetic can leave
          // a tiny angular residue; carrying that into the dormer is how a
          // supposedly rectilinear window becomes visibly skewed.
          for (const rewrite::DwellingWall &wall : plan.walls)
          {
            if (wall.id != facadeWall) continue;
            const double wx = wall.b.x - wall.a.x, wz = wall.b.z - wall.a.z;
            const double wl = std::hypot(wx, wz);
            if (wl > 1e-6)
            {
              double sx = wx / wl, sz = wz / wl;
              if (sx * ex + sz * ez < 0) { sx = -sx; sz = -sz; }
              ex = sx; ez = sz;
            }
            break;
          }

          const double edge0 = segmentCentre - widthFraction * .5;
          const double edge1 = segmentCentre + widthFraction * .5;
          const rewrite::Position centre{e0.x + (e1.x - e0.x) * segmentCentre,
                                         e0.y + (e1.y - e0.y) * segmentCentre,
                                         e0.z + (e1.z - e0.z) * segmentCentre};
          const double halfWidth = (edge1 - edge0) * span * .5;
          const rewrite::Position low0{centre.x - ex * halfWidth, centre.y, centre.z - ez * halfWidth};
          const rewrite::Position low1{centre.x + ex * halfWidth, centre.y, centre.z + ez * halfWidth};
          const double frontT = .16;
          double ux = -ez, uz = ex;
          const double towardX = peak.x - (e0.x + e1.x) * .5;
          const double towardZ = peak.z - (e0.z + e1.z) * .5;
          if (ux * towardX + uz * towardZ < 0) { ux = -ux; uz = -uz; }
          const double rectRun = ux * towardX + uz * towardZ;
          const double slope = rise / rectRun;
          const auto uphill = [&](const rewrite::Position &p, const double distance) {
            return rewrite::Position{p.x + ux * distance,
                                     p.y + slope * distance,
                                     p.z + uz * distance};
          };

          // A rectilinear gabled dormer: its front and window are parallel to
          // the eaves/wall segment and its two side axes remain parallel. The
          // cheek eaves and the end of its ridge are separate intersections
          // with the main slope, so the ridge enters the cut rather than
          // ending in a rear box.
          const double frontDistance = rectRun * frontT;
          const double wallHeight = std::min(settings.dormerWallMetres, slope * settings.dormerDepthMetres);
          const double ceilingDistance = frontDistance + wallHeight / slope;
          const double roofHeight = std::min(settings.dormerRoofMetres, slope * std::max(0., rectRun * .84 - ceilingDistance));
          const double ridgeDistance = ceilingDistance + roofHeight / slope;

          if (wallHeight < .32 || roofHeight < .10 || ridgeDistance >= rectRun * .88)
          {
            plan.roofs.push_back(roof);
            continue;
          }

          const rewrite::Position fl = uphill(low0, frontDistance), fr = uphill(low1, frontDistance);
          const rewrite::Position bl = uphill(low0, ceilingDistance), br = uphill(low1, ceilingDistance);
          const rewrite::Position rl = uphill(low0, ridgeDistance), rr = uphill(low1, ridgeDistance);
          const double ridgeY = std::round(((fl.y + fr.y) * .5 + wallHeight + roofHeight) * 1000.) / 1000.;
          const rewrite::Position ba{(rl.x + rr.x) * .5, ridgeY, (rl.z + rr.z) * .5};

          // A triangular hip narrows uphill. Fitting the dormer's width on
          // the eave alone does not guarantee its rear corners fit that face.
          // Cutting past a hip edge produces crossed replacement polygons.
          const auto carriedByRoof=[&](const rewrite::Position &p) {
            bool positive=false,negative=false;
            for (size_t k=0;k<roof.polygon.size();++k) {
              const auto a=roof.polygon[k],b=roof.polygon[(k+1)%roof.polygon.size()];
              const double side=(b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);
              // Leave a real strip beside hip edges. An aperture exactly on
              // an edge produces a near-zero sliver which can cross itself
              // after structural coordinates are quantized.
              if (std::abs(side)<.02*std::hypot(b.x-a.x,b.z-a.z)) return false;
              positive |= side>1e-7; negative |= side<-1e-7;
            }
            return !(positive && negative);
          };
          if (!carriedByRoof(fl) || !carriedByRoof(fr) || !carriedByRoof(bl) || !carriedByRoof(br) || !carriedByRoof(ba))
          {
            plan.roofs.push_back(roof);
            continue;
          }

          // A second roof plane crossing this aperture would clip the dormer
          // even though its own carrying face is correctly cut. This occurs
          // around valleys and turned wings; leave those junctions clear.
          const auto insideXZ = [](const std::vector<rewrite::Position> &ring, const rewrite::Position &p) {
            bool in = false;
            for (size_t a = 0, b = ring.size() - 1u; a < ring.size(); b = a++)
            {
              const rewrite::Position &u = ring[a], &v = ring[b];
              if ((u.z > p.z) != (v.z > p.z) && p.x < (v.x - u.x) * (p.z - u.z) / (v.z - u.z) + u.x) in = !in;
            }
            return in;
          };
          bool crossed = false;
          for (size_t other = 0; other < sourceRoofs.size() && !crossed; ++other)
          {
            const rewrite::DwellingRoofFace &candidate = sourceRoofs[other];
            if (other == sourceRoofIndex || candidate.gable || candidate.soffit || candidate.polygon.size() < 3u) continue;
            for (const rewrite::Position &q : std::array<rewrite::Position, 6>{fl, fr, bl, br, ba,
                 rewrite::Position{(fl.x + fr.x + bl.x + br.x) * .25, 0, (fl.z + fr.z + bl.z + br.z) * .25}})
              crossed = crossed || insideXZ(candidate.polygon, q);
          }
          if (crossed) { plan.roofs.push_back(roof); continue; }
          const auto raised = [](rewrite::Position p, const double amount) { p.y += amount; return p; };
          const rewrite::Position flt = raised(fl, wallHeight);
          const rewrite::Position frt = raised(fr, wallHeight);
          const rewrite::Position fa{(flt.x + frt.x) * .5, ridgeY,
                                     (flt.z + frt.z) * .5};

          rewrite::DwellingDormer dormer;
          dormer.facadeWall = facadeWall;
          dormer.opening = {fl, fr, br, ba, bl};
          dormer.walls = {{fl, fr, frt, fa, flt}, {fr, br, frt}, {bl, fl, flt}};
          dormer.roofFaces = {{flt, fa, ba, bl}, {fa, frt, br, ba}};
          dormer.ceiling = {flt, frt, br, bl};

          // Preserve the carrying triangle's winding in each replacement.
          const auto normal = [](const std::vector<rewrite::Position> &p) {
            rewrite::Position n{};
            for (size_t i = 0; i < p.size(); ++i)
            {
              const rewrite::Position &a = p[i], &b = p[(i + 1u) % p.size()];
              n.x += (a.y - b.y) * (a.z + b.z);
              n.y += (a.z - b.z) * (a.x + b.x);
              n.z += (a.x - b.x) * (a.y + b.y);
            }
            return n;
          };
          const rewrite::Position roofNormal = normal(roof.polygon);
          const auto appendRoof = [&](std::vector<rewrite::Position> polygon) {
            const rewrite::Position n = normal(polygon);
            if (n.x * roofNormal.x + n.y * roofNormal.y + n.z * roofNormal.z < 0)
            {
              std::reverse(polygon.begin(), polygon.end());
            }
            rewrite::DwellingRoofFace cut = roof;
            cut.polygon = std::move(polygon);
            dormer.roofs.push_back(uint32_t(plan.roofs.size()));
            plan.roofs.push_back(std::move(cut));
          };

          appendRoof({e0, e1, fr, fl});
          appendRoof({e0, fl, bl, ba, peak});
          appendRoof({fr, e1, peak, ba, br});

          const rewrite::Position wl{fl.x - ux * .015, fl.y + .18, fl.z - uz * .015};
          const rewrite::Position wr{fr.x - ux * .015, fr.y + .18, fr.z - uz * .015};
          dormer.window = {wl, wr, raised(wr, wallHeight * .58), raised(wl, wallHeight * .58)};
          plan.dormers.push_back(std::move(dormer));
        }
      }
    }
  }

  // Interiors remain unfurnished until purpose-built furniture models are available.

  // Legacy corner drums have no reserved room volume and cannot be meshed
  // safely as circulation. Replan them through rollStairs' integrated tower.
  if (house.spiral.has_value())
  {
    if (error) *error = "unreserved corner turret requires cell-plan regeneration";
    return false;
  }

  // ---- the bell tower ------------------------------------------------------
  //
  // Two towers, one on the other. The square shaft is already built - its walls
  // are the belfry room's, carried to the deck height the planner settled - so
  // what is left is everything above and inside it: the deck that closes the
  // shaft, the hole through that deck, the six-sided chamber standing on it
  // with the bells in it, its cap, and the climb.
  for (const CellBellTower &tower : house.bellTowers)
  {
    if (tower.cells.empty() || tower.topMetres <= tower.deckMetres)
    {
      continue;
    }

    const double deck = tower.deckMetres;
    const double top = tower.topMetres;
    const double radius = tower.radius;
    const uint32_t sides = std::max(3u, settings.bellChamberSides);
    // Author the complete tower assembly in its shaft's local frame.
    const double cx = 0, cz = 0;
    const auto local=[&](rewrite::Position p) {
      const double x=p.x-tower.x,z=p.z-tower.z;
      return rewrite::Position{x*house.frame.axisX+z*house.frame.axisZ,p.y,
                               -x*house.frame.axisZ+z*house.frame.axisX};
    };
    const auto world=[&](rewrite::Position &p) {
      const double x=p.x,z=p.z;
      p.x=tower.x+x*house.frame.axisX-z*house.frame.axisZ;
      p.z=tower.z+x*house.frame.axisZ+z*house.frame.axisX;
    };
    const size_t firstRoof=plan.roofs.size(),firstWall=plan.walls.size(),firstOpening=plan.openings.size(),
                 firstDoor=plan.doors.size(),firstRoom=plan.rooms.size(),firstStair=plan.stairs.size(),firstTimber=plan.timbers.size();

    // The shaft, in metres: the inside face of its walls, which is what the
    // deck spans and what the climb hugs.
    double leastX = 1e300, mostX = -1e300, leastZ = 1e300, mostZ = -1e300;

    for (const auto &[i, j] : geom2::cells(tower.cells))
    {
      double x = 0;
      double z = 0;
      geom2::cellPoint(house.grid, house.frame, house.grid.cell(int32_t(i), int32_t(j)), x, z);

      const auto p=local({x,0,z});
      leastX = std::min(leastX, p.x);
      mostX = std::max(mostX, p.x);
      leastZ = std::min(leastZ, p.z);
      mostZ = std::max(mostZ, p.z);
    }

    const double half = house.frame.cellMetres * .5;
    const double x0 = leastX - half, x1 = mostX + half;
    const double z0 = leastZ - half, z1 = mostZ + half;

    // ---- the climb ----------------------------------------------------------
    //
    // Up the inside of the square shaft to the hole in the deck. Two ways, and
    // the difference between them is the difference between a town church and a
    // parish one rather than a difference of drawing: a stair winds round the
    // wall in quarter turns, and a ladder simply leans against it.
    const uint32_t storeyIndex = uint32_t(house.basement.has_value() ? 1 : 0);
    const double climb = deck;

    // Where the climb ARRIVES on the deck. The hole is cut there, not at the
    // middle: a wall stair and a ladder both end against the shaft's wall, and
    // a hole in the middle of the deck was a hole nothing reached.
    double arriveX = cx, arriveZ = cz;

    const double inset = settings.outerWallMetres * .5;
    const rewrite::Position ring[4] = {{x0 + inset, 0, z0 + inset},
                                       {x1 - inset, 0, z0 + inset},
                                       {x1 - inset, 0, z1 - inset},
                                       {x0 + inset, 0, z1 - inset}};

    if (climb > 2.)
    {
      if (tower.ladder)
      {
        // A ladder: two stiles and the rungs between them, against one wall and
        // ending at the hole. Built as timber rather than as a steep flight,
        // because it is not a flight - nothing about a stair's landings, going
        // or width says anything true about it.
        const rewrite::Position &from = ring[0];
        const rewrite::Position &to = ring[1];
        const double run = std::hypot(to.x - from.x, to.z - from.z);

        if (run > 1.2)
        {
          const double dx = (to.x - from.x) / run, dz = (to.z - from.z) / run;
          const double nx = -dz, nz = dx;

          // Leaning: the foot stands out from the wall, the head is at it.
          const double width = .52;
          const double lean = .55;
          const double mid = run * .5;

          arriveX = from.x + dx * mid + nx * (.12 + .45);
          arriveZ = from.z + dz * mid + nz * (.12 + .45);

          for (int32_t side = -1; side <= 1; side += 2)
          {
            const double off = double(side) * width * .5;

            rewrite::DwellingTimber stile;
            stile.floor = storeyIndex;
            stile.kind = rewrite::TimberKind::Post;
            stile.a = rewrite::Position{from.x + dx * (mid + off) + nx * lean, 0,
                                        from.z + dz * (mid + off) + nz * lean};
            stile.b = rewrite::Position{from.x + dx * (mid + off) + nx * .12, climb,
                                        from.z + dz * (mid + off) + nz * .12};
            stile.width = .07;
            stile.depth = .07;
            plan.timbers.push_back(std::move(stile));
          }

          const uint32_t rungs = uint32_t(std::max(4., std::floor(climb / .30)));

          for (uint32_t k = 1; k < rungs; ++k)
          {
            const double t = double(k) / double(rungs);
            const double out = lean + (.12 - lean) * t;

            rewrite::DwellingTimber rung;
            rung.floor = storeyIndex;
            rung.kind = rewrite::TimberKind::Rail;
            rung.a = rewrite::Position{from.x + dx * (mid - width * .5) + nx * out, climb * t,
                                       from.z + dz * (mid - width * .5) + nz * out};
            rung.b = rewrite::Position{from.x + dx * (mid + width * .5) + nx * out, climb * t,
                                       from.z + dz * (mid + width * .5) + nz * out};
            rung.width = .05;
            rung.depth = .04;
            plan.timbers.push_back(std::move(rung));
          }
        }
      }
      else
      {
        const double width = settings.towerStairWidthMetres;

        uint32_t legs = 0;

        {
          double rise = 0;
          const double side = std::min(x1 - x0, z1 - z0);
          const double run = std::max(0., side - width * 2.);
          const double each = std::max(.6, run * .6);

          while (rise < climb && legs < 64u)
          {
            rise += each;
            ++legs;
          }
        }

        const double perLeg = legs > 0 ? climb / double(legs) : 0.;

        for (uint32_t leg = 0; leg < legs; ++leg)
        {
          const rewrite::Position &from = ring[leg % 4u];
          const rewrite::Position &to = ring[(leg + 1u) % 4u];

          const double run = std::hypot(to.x - from.x, to.z - from.z);

          if (run < width * 2. + 1.)
          {
            continue;
          }

          const double dx = (to.x - from.x) / run, dz = (to.z - from.z) / run;

          // The ring runs so that into the shaft is to the left of it.
          const double nx = -dz, nz = dx;

          const auto corner = [&](const double along, const double into) {
            return rewrite::Position{from.x + dx * along + nx * into, 0, from.z + dz * along + nz * into};
          };

          const double foot = width, headOf = run - width;
          const StairSteps steps = stepsFor(perLeg, settings.towerRiserMetres, headOf - foot);
          const double climbed = headOf - foot;

          rewrite::DwellingStair flight;
          flight.opening = {corner(foot, width), corner(foot, 0), corner(foot + climbed, 0),
                            corner(foot + climbed, width)};
          flight.lowerLanding = {corner(0, width), corner(0, 0), corner(foot, 0), corner(foot, width)};
          flight.upperLanding = {corner(headOf, width), corner(headOf, 0), corner(run, 0), corner(run, width)};
          flight.lowerFloor = storeyIndex;
          flight.risers = steps.risers;
          flight.tread = climbed / double(steps.risers);
          flight.rise = perLeg;
          flight.startRise = perLeg * double(leg);
          flight.width = width;
          flight.privateAccess = true;
          flight.wallSupported = true;
          // The previous head landing is this flight's foot landing.
          if (leg > 0) flight.lowerLanding.clear();
          // Two slender stringers carry the tread blocks between wall-fixed
          // corner platforms, leaving the circulation below unobstructed.
          for (double across : {.10, width-.10}) {
            rewrite::DwellingTimber beam;
            beam.floor=storeyIndex;beam.kind=rewrite::TimberKind::Rail;
            beam.a=corner(foot,across);beam.b=corner(headOf,across);
            beam.a.y=flight.startRise-.23;beam.b.y=flight.startRise+perLeg-.23;
            beam.width=.14;beam.depth=.24;plan.timbers.push_back(beam);
          }
          plan.stairs.push_back(std::move(flight));

          // The last leg's head is where you come out.
          const rewrite::Position headAt = corner(foot + climbed + .3, width * .5);
          arriveX = headAt.x;
          arriveZ = headAt.z;
        }
      }
    }

    // The hole you come up through, where the climb arrives, kept a wall's
    // thickness inside the shaft.
    const double hole = std::min(1.1, std::min(x1 - x0, z1 - z0) * .3);
    const double hcx = std::clamp(arriveX, x0 + settings.outerWallMetres + hole * .5, x1 - settings.outerWallMetres - hole * .5);
    const double hcz = std::clamp(arriveZ, z0 + settings.outerWallMetres + hole * .5, z1 - settings.outerWallMetres - hole * .5);
    const auto deckPolygon=[](const std::vector<rewrite::Position> &ring) {
      geom2::Polygon p;for(auto q:ring)p.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});
      if(!geom2::counterClockwise(p.outer))geom2::reverse(p.outer);return p;
    };
    std::vector<geom2::Operand> deckCuts;
    if(tower.ladder)deckCuts.push_back(geom2::toOperand(deckPolygon({
      {hcx-hole*.5,0,hcz-hole*.5},{hcx+hole*.5,0,hcz-hole*.5},
      {hcx+hole*.5,0,hcz+hole*.5},{hcx-hole*.5,0,hcz+hole*.5}})));
    for(size_t i=firstStair;i<plan.stairs.size();++i) {
      const auto &flight=plan.stairs[i];
      if(flight.startRise+flight.rise < deck-2.2)continue;
      auto opening=flight.opening;
      const double t=std::clamp((deck-2.2-flight.startRise)/flight.rise,0.,1.);
      for(size_t k=0;k<2;++k) {
        const auto end=opening[3-k];
        opening[k].x+=(end.x-opening[k].x)*t;
        opening[k].z+=(end.z-opening[k].z)*t;
      }
      deckCuts.push_back(geom2::toOperand(deckPolygon(opening)));
      // The arrival pad supplies its own surface at deck height. Subtract
      // it too, so it shares an edge with the deck instead of z-fighting.
      deckCuts.push_back(geom2::toOperand(deckPolygon(flight.upperLanding)));
    }

    // Replace the shaft's generic tiled ceiling: it otherwise seals the
    // stair opening and lies exactly on the bell deck (visible z-fighting).
    plan.ceilings.erase(std::remove_if(plan.ceilings.begin(), plan.ceilings.end(), [&](const auto &ring) {
      return !ring.empty() && std::all_of(ring.begin(), ring.end(), [&](const auto &point) {
        const auto p=local(point);
        return std::abs(p.y-deck)<1e-6 && p.x>=x0-1e-6 && p.x<=x1+1e-6 &&
               p.z>=z0-1e-6 && p.z<=z1+1e-6;
      });
    }), plan.ceilings.end());

    const size_t firstCeiling=plan.ceilings.size();

    // Subtract the actual headroom envelope, preserving the central deck
    // and its path into the bell chamber rather than cutting a large box.
    {
      auto footprint=deckPolygon({{x0,0,z0},{x1,0,z0},{x1,0,z1},{x0,0,z1}});
      std::vector<geom2::Polygon> pieces;std::string why;
      if(!geom2::differenceOf(geom2::toOperand(footprint),deckCuts,pieces,&why)) {
        if(error)*error="bell deck cutout: "+why;return false;
      }
      for(const auto &piece:pieces) {
        std::vector<geom2::Point> vertices;std::vector<std::array<uint32_t,3>> triangles;
        if(!geom3::triangulate(piece,triangles,vertices,&why)) {
          if(error)*error="bell deck triangulation: "+why;return false;
        }
        for(auto tri:triangles) {
          rewrite::DwellingRoofFace face;face.wing=uint32_t(house.floors.size());
          for(auto k:tri)face.polygon.push_back({geom2::toMetres(vertices[k].x),deck,geom2::toMetres(vertices[k].y)});
          auto underside=face.polygon;for(auto &p:underside)p.y-=settings.floorBoardMetres;
          std::reverse(face.polygon.begin(),face.polygon.end());
          plan.ceilings.push_back(std::move(underside));plan.roofs.push_back(std::move(face));
        }
      }
    }

    // The chamber: one wall per side of the drum, each with a bell opening in
    // it. Not piers and a course - that was a lantern drawn in full, and the
    // request was the simple version: six faces, each rendered like a windowed
    // wall, so a later mesh step can put the arcade on them if it wants one.
    // They are WALLS of the plan, with WINDOWS cut in them, so the structural
    // step types them itself and nothing here says how they are drawn.
    const double head = std::max(.4, settings.bellStageHeadMetres);
    const double stage = std::max(1.2, top - deck - head);
    const double pi = 3.14159265358979323846;
    const uint32_t chamberFloor = uint32_t(house.floors.size() - 1 + (house.basement.has_value() ? 1 : 0));

    // The drum is an occupied volume, not a set of freestanding walls.
    // Without this room, the structural mesher sees outside on BOTH skins
    // and offsets the masonry outward, beyond its roof and around its doors.
    rewrite::DwellingRoom chamber;
    chamber.id=1;
    for(const auto &room:plan.rooms)chamber.id=std::max(chamber.id,room.id+1);
    chamber.floor=chamberFloor;
    chamber.kind=rewrite::RoomKind::Belfry;
    chamber.ceilingHeight=deck+stage+head;
    for(uint32_t k=0;k<sides;++k) {
      const double a=2.*pi*double(k)/double(sides);
      chamber.boundary.push_back({cx+radius*std::cos(a),deck,cz+radius*std::sin(a)});
    }
    const double innerRadius=std::max(0.,radius-settings.outerWallMetres/std::cos(pi/sides));
    chamber.usableArea=.5*sides*innerRadius*innerRadius*std::sin(2.*pi/sides);
    const auto chamberId=chamber.id;
    auto chamberCeiling=chamber.boundary;
    for(auto &p:chamberCeiling)p.y=chamber.ceilingHeight;
    plan.ceilings.push_back(std::move(chamberCeiling));
    plan.rooms.push_back(std::move(chamber));

    // The side facing the hole is the way in: you come up onto the deck
    // outside the drum and step in through a doorway, because the drum is
    // inscribed in the shaft and every point along the shaft's wall - where
    // a wall stair or a ladder arrives - is outside its ring.
    uint32_t doorSide = 0;
    double nearest = 1e300;

    for (uint32_t k = 0; k < sides; ++k)
    {
      const double a = 2. * pi * (double(k) + .5) / double(sides);
      const double mx = cx + radius * std::cos(a), mz = cz + radius * std::sin(a);
      const double reach = std::hypot(mx - hcx, mz - hcz);

      if (reach < nearest)
      {
        nearest = reach;
        doorSide = k;
      }
    }

    for (uint32_t k = 0; k < sides; ++k)
    {
      const double a0 = 2. * pi * double(k) / double(sides);
      const double a1 = 2. * pi * double(k + 1) / double(sides);

      rewrite::DwellingWall side;
      side.id = uint64_t(plan.walls.size()) + 1;
      side.floor = chamberFloor;
      side.a = rewrite::Position{cx + radius * std::cos(a0), deck, cz + radius * std::sin(a0)};
      side.b = rewrite::Position{cx + radius * std::cos(a1), deck, cz + radius * std::sin(a1)};
      side.thickness = settings.outerWallMetres;
      side.exterior = true;
      side.kind = rewrite::WallKind::Solid;
      side.height = stage + head;
      plan.walls.push_back(side);

      // The opening: most of the side, from a low sill to under the head.
      rewrite::DwellingOpening bell;
      bell.wall = side.id;
      bell.room = chamberId;
      bell.floor = chamberFloor;
      bell.a = rewrite::Position{side.a.x + (side.b.x - side.a.x) * .2, deck, side.a.z + (side.b.z - side.a.z) * .2};
      bell.b = rewrite::Position{side.a.x + (side.b.x - side.a.x) * .8, deck, side.a.z + (side.b.z - side.a.z) * .8};
      bell.base = k == doorSide ? 0. : .5;
      bell.top = stage;
      bell.sill = bell.base;
      bell.head = stage;
      bell.thickness = side.thickness;
      bell.kind = k == doorSide ? rewrite::OpeningKind::Door : rewrite::OpeningKind::Window;
      bell.exterior = true;
      plan.openings.push_back(bell);

      if (k == doorSide)
      {
        rewrite::DwellingDoor leaf;
        leaf.wall = side.id;
        leaf.room = chamberId;
        leaf.floor = chamberFloor;
        leaf.center = rewrite::Position{(side.a.x + side.b.x) * .5, deck, (side.a.z + side.b.z) * .5};
        leaf.width = std::hypot(bell.b.x - bell.a.x, bell.b.z - bell.a.z);
        leaf.privateAccess = false;
        plan.doors.push_back(leaf);
      }
    }

    // The cap.
    for (uint32_t k = 0; k < sides; ++k)
    {
      const double a0 = 2. * pi * double(k) / double(sides);
      const double a1 = 2. * pi * double(k + 1) / double(sides);

      rewrite::DwellingRoofFace cap;
      cap.wing = uint32_t(house.floors.size());
      cap.gable = false;
      cap.radial = true;
      cap.polygon = {
        rewrite::Position{cx + radius * std::cos(a0), deck + stage + head, cz + radius * std::sin(a0)},
        rewrite::Position{cx + radius * std::cos(a1), deck + stage + head, cz + radius * std::sin(a1)},
        rewrite::Position{cx, deck + stage + head + settings.bellChamberCapMetres, cz}};
      std::reverse(cap.polygon.begin(),cap.polygon.end());
      plan.roofs.push_back(std::move(cap));
    }

    const auto transformRing=[&](auto &ring){for(auto &p:ring)world(p);};
    for(size_t i=firstRoof;i<plan.roofs.size();++i)transformRing(plan.roofs[i].polygon);
    for(size_t i=firstCeiling;i<plan.ceilings.size();++i)transformRing(plan.ceilings[i]);
    for(size_t i=firstRoom;i<plan.rooms.size();++i)transformRing(plan.rooms[i].boundary);
    for(size_t i=firstWall;i<plan.walls.size();++i){world(plan.walls[i].a);world(plan.walls[i].b);}
    for(size_t i=firstOpening;i<plan.openings.size();++i){world(plan.openings[i].a);world(plan.openings[i].b);}
    for(size_t i=firstDoor;i<plan.doors.size();++i)world(plan.doors[i].center);
    for(size_t i=firstTimber;i<plan.timbers.size();++i){world(plan.timbers[i].a);world(plan.timbers[i].b);}
    for(size_t i=firstStair;i<plan.stairs.size();++i){auto &stair=plan.stairs[i];
      transformRing(stair.opening);transformRing(stair.lowerLanding);transformRing(stair.upperLanding);}

  }

  // ---- the balcony ---------------------------------------------------------
  //
  // On the back, because that is where one goes. The front of a house of this
  // age is its frontage and is worth money; the gallery, the stair to it and
  // the privy are all on the yard side, and a balcony over the street is a
  // thing a later century builds.
  //
  // Not part of any storey plate, and deliberately: a plate is where the walls
  // are, and the whole of what makes a balcony a balcony is that there are
  // none. So it is built after the massing, out of posts, decking and a rail.
  if (settings.balconyDepthMetres > .5 && house.floors.size() >= 2)
  {
    const CellFloor &upper = house.floors[1];
    const std::vector<WallRun> runs = wallRuns(house.grid, upper.loops.front());

    const WallRun *chosen = nullptr;
    double longest = 0;

    for (const WallRun &run : runs)
    {
      double dj = 0;
      double di = 0;
      outwardStep(run.dir, dj, di);

      // The yard side: away from the street.
      if (streetLength > 1e-9 && (dj * house.streetJ + di * house.streetI) / streetLength > -.5)
      {
        continue;
      }

      // And with open air outside it, all the way down.
      //
      // A balcony stands on posts, and posts stand on the GROUND. A wall of an
      // upper floor that has stepped back has the roof of the floor below
      // outside it, not air - so a deck hung there would have its posts coming
      // up through those tiles. Exactly the fault the jetty has to avoid, in a
      // different costume, and the cell model refuses both the same way.
      bool openAir = true;

      for (const uint32_t e : run.edges)
      {
        const uint32_t outside = house.grid.neighbour(house.grid.edgeCell(e), ccw(run.dir));

        if (outside != geom2::kNoCell && house.grid.contains(house.floors.front().area, outside))
        {
          openAir = false;
          break;
        }
      }

      if (!openAir)
      {
        continue;
      }

      const double length = runLengthMetres(house.grid, house.frame, run);

      if (length > longest && length >= settings.balconyMinimumRunMetres)
      {
        longest = length;
        chosen = &run;
      }
    }

    // A deck is admitted only with an exterior wall bay opening onto it.
    const rewrite::DwellingWall *balconyWall=nullptr;
    if(chosen) {
      double ax,az,bx,bz;geom2::nodePoint(house.grid,house.frame,chosen->startNode,ax,az);
      geom2::nodePoint(house.grid,house.frame,chosen->endNode,bx,bz);
      const double dx=(bx-ax)/longest,dz=(bz-az)/longest;
      const double span=std::max(settings.balconyMinimumRunMetres,longest*settings.balconyShare);
      double best=1e30;
      for(const auto &wall:plan.walls) {
        if(!wall.exterior || wall.kind!=rewrite::WallKind::Solid ||
           wall.floor!=uint32_t(upper.level+(house.basement.has_value()?1:0)))continue;
        const double x=(wall.a.x+wall.b.x)*.5,z=(wall.a.z+wall.b.z)*.5;
        const double along=(x-ax)*dx+(z-az)*dz,away=std::abs((x-ax)*dz-(z-az)*dx);
        const double width=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z);
        if(away>.01 || std::abs(along-longest*.5)+settings.doorWidthMetres*.5>span*.5 || width<settings.doorWidthMetres+.09)continue;
        bool supported=false;
        if(wall.floor<plan.floors.size())for(double side:{-.35,.35}) {
          const double px=x-dz*side,pz=z+dx*side;
          for(const auto &slab:plan.floors[wall.floor].slabs) {
            bool in=false;
            for(size_t i=0,j=slab.size()?slab.size()-1:0;i<slab.size();j=i++) {
              const auto a=slab[i],b=slab[j];
              if((a.z>pz)!=(b.z>pz) && px<(b.x-a.x)*(pz-a.z)/(b.z-a.z)+a.x)in=!in;
            }
            supported |= in;
          }
        }
        if(!supported)continue; // Never put a balcony door across a stair void.

        if(std::abs(along-longest*.5)<best){best=std::abs(along-longest*.5);balconyWall=&wall;}
      }
    }
    if (chosen != nullptr && balconyWall != nullptr)
    {
      const double deck = double(upper.level) * storeyHeight;
      const double board = deck + settings.floorBoardMetres;
      const double depth = settings.balconyDepthMetres;
      const double span = std::max(settings.balconyMinimumRunMetres, longest * settings.balconyShare);
      const double from = (longest - span) * .5;
      const auto wall=*balconyWall;
      const double wallLength=std::hypot(wall.b.x-wall.a.x,wall.b.z-wall.a.z);
      const rewrite::Position centre{(wall.a.x+wall.b.x)*.5,deck,(wall.a.z+wall.b.z)*.5};
      rewrite::DwellingOpening doorway;
      doorway.wall=wall.id;doorway.floor=wall.floor;doorway.kind=rewrite::OpeningKind::Door;
      doorway.exterior=true;doorway.thickness=wall.thickness;doorway.base=doorway.sill=0;
      doorway.top=doorway.head=std::min(2.3,storeyHeight-.25);
      const double half=settings.doorWidthMetres*.5/wallLength;
      doorway.a={centre.x-(wall.b.x-wall.a.x)*half,deck,centre.z-(wall.b.z-wall.a.z)*half};
      doorway.b={centre.x+(wall.b.x-wall.a.x)*half,deck,centre.z+(wall.b.z-wall.a.z)*half};
      for(const auto &o:plan.openings)if(o.wall==wall.id)doorway.room=o.room;
      plan.openings.erase(std::remove_if(plan.openings.begin(),plan.openings.end(),[&](const auto &o){return o.wall==wall.id;}),plan.openings.end());
      plan.openings.push_back(doorway);
      rewrite::DwellingDoor access;access.wall=wall.id;access.floor=wall.floor;
      access.center=centre;access.width=settings.doorWidthMetres;access.exterior=true;
      uint32_t local=0;for(const auto &room:plan.rooms)if(room.floor==wall.floor){if(local++==doorway.room){access.room=room.id;break;}}
      plan.doors.push_back(access);
      plan.furniture.erase(std::remove_if(plan.furniture.begin(),plan.furniture.end(),[&](const auto &f){
        if(f.floor!=wall.floor)return false;
        for(auto p:f.footprint)if(std::hypot(p.x-centre.x,p.z-centre.z)<1.4)return true;
        return false;
      }),plan.furniture.end());


      // A frame for the deck: along the wall, and out from it.
      double alongX = 0;
      double alongZ = 0;
      double outX = 0;
      double outZ = 0;
      {
        double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        geom2::nodePoint(house.grid, house.frame, chosen->startNode, x0, z0);
        geom2::nodePoint(house.grid, house.frame, chosen->endNode, x1, z1);
        const double length = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));

        if (length > 1e-9)
        {
          alongX = (x1 - x0) / length;
          alongZ = (z1 - z0) / length;
        }

        // Out is the quarter turn anticlockwise from along, in the same sense
        // the contour uses.
        outX = alongZ;
        outZ = -alongX;

        const auto at = [&](const double t, const double d, const double y) {
          return rewrite::Position{x0 + alongX * t + outX * d, y, z0 + alongZ * t + outZ * d};
        };

        // The decking, as two triangles, exactly as the existing planner emits
        // one so that anything reading a plan does not have to learn a second
        // shape for the same thing.
        const size_t level = size_t(upper.level) + (house.basement.has_value() ? 1 : 0);

        if (level < plan.floors.size())
        {
          plan.floors[level].slabs.push_back({at(from, 0, board), at(from + span, 0, board), at(from + span, depth, board)});
          plan.floors[level].slabs.push_back({at(from, 0, board), at(from + span, depth, board), at(from, depth, board)});
        }

        // The posts, along the front of the deck and standing on the ground.
        const int32_t bays = std::max(1, int32_t(std::lround(span / std::max(settings.balconyPostSpacingMetres, .6))));
        const double half = settings.balconyPostSideMetres * .5;

        for (int32_t b = 0; b <= bays; ++b)
        {
          const double t = from + span * double(b) / double(bays);
          const rewrite::Position foot = at(t, depth - half, 0);

          rewrite::DwellingPillar post;
          post.base = 0;
          post.top = deck;
          post.footprint = {
            rewrite::Position{foot.x - alongX * half - outX * half, 0, foot.z - alongZ * half - outZ * half},
            rewrite::Position{foot.x + alongX * half - outX * half, 0, foot.z + alongZ * half - outZ * half},
            rewrite::Position{foot.x + alongX * half + outX * half, 0, foot.z + alongZ * half + outZ * half},
            rewrite::Position{foot.x - alongX * half + outX * half, 0, foot.z - alongZ * half + outZ * half}};
          plan.pillars.push_back(std::move(post));
        }

        // The rail: along the front and round both returns. A balustrade is a
        // wall in the plan, open rather than solid, because that is what it is.
        const rewrite::Position corners[4] = {
          at(from, 0, board), at(from, depth, board), at(from + span, depth, board), at(from + span, 0, board)};

        for (uint32_t k = 0; k < 3; ++k)
        {
          rewrite::DwellingWall rail;
          rail.id = plan.walls.size() + 1;
          rail.floor = uint32_t(level);
          rail.a = corners[k];
          rail.b = corners[k + 1];
          rail.thickness = settings.balconyPostSideMetres;
          rail.exterior = true;
          rail.kind = rewrite::WallKind::Open;
          rail.height = settings.balconyRailHeightMetres;
          plan.walls.push_back(std::move(rail));
        }
      }
    }
  }

  // Apply the same frontage displacement to the room and wall topology.
  // Moving only storey.outer leaves the occupied rooms at the old facade and
  // produces an outdoor shelf. Shared partition endpoints, openings and roof
  // edges must follow the identical mapping before structural generation.
  const auto projectPoint=[&](uint32_t level,rewrite::Position &p,double eaves=0.) {
    const auto found=projectedRuns.find(level);if(found==projectedRuns.end())return;
    double moveX=0,moveZ=0;
    for(const auto &run:found->second)if(run.project>0) {
      double ax,az,bx,bz;geom2::nodePoint(house.grid,house.frame,run.startNode,ax,az);geom2::nodePoint(house.grid,house.frame,run.endNode,bx,bz);
      const double length=std::hypot(bx-ax,bz-az);if(length<1e-8)continue;
      double dj,di,ox,oz,ex,ez;outwardStep(run.dir,dj,di);
      geom2::framePoint(house.frame,0,0,ox,oz);geom2::framePoint(house.frame,dj,di,ex,ez);
      const double nx=(ex-ox)/house.frame.cellMetres,nz=(ez-oz)/house.frame.cellMetres;
      const double along=((p.x-ax)*(bx-ax)+(p.z-az)*(bz-az))/length;
      const double outward=(p.x-ax)*nx+(p.z-az)*nz;
      if(along>=-eaves-1e-5 && along<=length+eaves+1e-5 && outward>=-1e-5 && outward<=eaves+1e-5) {
        moveX+=nx*run.project*house.frame.cellMetres;moveZ+=nz*run.project*house.frame.cellMetres;
      }
    }
    p.x+=moveX;p.z+=moveZ;
  };
  const auto projectRing=[&](uint32_t level,auto &ring,double eaves=0.){
    // Room contours collapse collinear lattice nodes. Restore a run endpoint
    // when it lies on such an edge, otherwise moving only its distant corners
    // shears the room and loses part of the newly enclosed frontage.
    const auto found=projectedRuns.find(level);
    if(found!=projectedRuns.end() && eaves==0) {
      std::vector<rewrite::Position> split;
      for(size_t k=0;k<ring.size();++k) {
        const auto a=ring[k],b=ring[(k+1)%ring.size()];split.push_back(a);
        const double dx=b.x-a.x,dz=b.z-a.z,dd=dx*dx+dz*dz;if(dd<1e-12)continue;
        std::vector<double> cuts;
        // Both sides of a room partition need identical vertices. A T-junction
        // may retain a middle node on one room but simplify it away on the
        // other; stretching the unsplit edge would create gaps/overlap.
        const double length=std::sqrt(dd);
        const double u=dx*house.frame.axisX+dz*house.frame.axisZ;
        const double v=-dx*house.frame.axisZ+dz*house.frame.axisX;
        if(std::abs(u)<1e-6 || std::abs(v)<1e-6)
          for(double d=house.frame.cellMetres;d<length-1e-6;d+=house.frame.cellMetres)cuts.push_back(d/length);
        for(const auto &run:found->second)if(run.project>0)for(auto node:{run.startNode,run.endNode}) {
          double x,z;geom2::nodePoint(house.grid,house.frame,node,x,z);
          const double t=((x-a.x)*dx+(z-a.z)*dz)/dd;
          if(t>1e-7 && t<1-1e-7 && std::hypot(x-a.x-t*dx,z-a.z-t*dz)<1e-6)cuts.push_back(t);
        }
        std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end(),[](double a,double b){return std::abs(a-b)<1e-7;}),cuts.end());
        for(auto t:cuts)split.push_back({a.x+t*dx,a.y+t*(b.y-a.y),a.z+t*dz});
      }
      ring=std::move(split);
    }
    for(auto &p:ring)projectPoint(level,p,eaves);
  };
  const auto ringArea=[](const auto &ring){double area=0;for(size_t k=0;k<ring.size();++k){auto a=ring[k],b=ring[(k+1)%ring.size()];area+=a.x*b.z-b.x*a.z;}return std::abs(area)*.5;};
  for(auto &room:plan.rooms){const double before=ringArea(room.boundary);projectRing(room.floor,room.boundary);room.usableArea+=ringArea(room.boundary)-before;}
  for(auto &wall:plan.walls){projectPoint(wall.floor,wall.a);projectPoint(wall.floor,wall.b);}
  for(auto &opening:plan.openings){projectPoint(opening.floor,opening.a);projectPoint(opening.floor,opening.b);}
  for(auto &door:plan.doors)projectPoint(door.floor,door.center);
  for(size_t level=0;level<plan.floors.size();++level)for(auto &slab:plan.floors[level].slabs)projectRing(uint32_t(level),slab);
  for(auto &ceiling:plan.ceilings)if(!ceiling.empty()) {
    const int level=int(std::ceil((ceiling.front().y-1e-5)/storeyHeight))-1+(house.basement.has_value()?1:0);
    if(level>=0)projectRing(uint32_t(level),ceiling);
  }
  for(auto &roof:plan.roofs)projectRing(roof.wing+(house.basement.has_value()?1:0),roof.polygon,settings.eavesOverhangMetres+.001);
  for(auto &dormer:plan.dormers)if(!dormer.roofs.empty() && dormer.roofs.front()<plan.roofs.size()) {
    const auto level=plan.roofs[dormer.roofs.front()].wing+(house.basement.has_value()?1:0);
    for(auto &ring:dormer.walls)projectRing(level,ring,settings.eavesOverhangMetres+.001);
    for(auto &ring:dormer.roofFaces)projectRing(level,ring,settings.eavesOverhangMetres+.001);
    projectRing(level,dormer.window);projectRing(level,dormer.opening);projectRing(level,dormer.ceiling);
  }
  plan.ceilings.insert(plan.ceilings.end(),jettySoffits.begin(),jettySoffits.end());
  wallTurrets(house, settings, plan);
  finishDwellingStyle(house,plan);
  return true;
}

} // namespace procedural::world::generator
