#include "CompoundPlanner.hpp"

#include "DwellingPlanner.hpp"
#include "CellDwellingRealise.hpp"
#include "CellDwellingClasses.hpp"
#include "PatchTiling.hpp"
#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
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

bool fitCompoundPrecinct(const std::vector<ConvexRing> &pieces,double margin,ConvexRing &out,std::string *error,double minimumWidth,double minimumDepth)
{
  std::vector<geom2::Operand> operands;
  for(const auto &piece:pieces){geom2::Polygon p;for(auto v:piece)p.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});operands.push_back(geom2::toOperand(p));}
  std::vector<geom2::Polygon> joined;
  if(!geom2::unionOf(operands,joined,error))return false;
  std::vector<geom2::Polygon> inset;
  if(!geom2::offsetPolygons(joined,-margin,{},inset,error))return false;
  double best=0;out.clear();
  for(const auto &polygon:inset)for(unsigned rotation=0;rotation<12;++rotation) {
    // Reject impossible frames before rasterising every square metre. The
    // master planner tests many connected regions that cannot fit the estate.
    if(std::abs(geom2::areaMetres(polygon))+1e-6<minimumWidth*minimumDepth)continue;
    const double angle=rotation*3.141592653589793/12,nx=std::cos(angle),nz=std::sin(angle);
    double lowX=1e300,lowZ=1e300,highX=-1e300,highZ=-1e300;
    for(const auto &p:polygon.outer) {
      const double x=geom2::toMetres(p.x),z=geom2::toMetres(p.y);
      const double u=nx*x+nz*z,v=-nz*x+nx*z;
      lowX=std::min(lowX,u);highX=std::max(highX,u);
      lowZ=std::min(lowZ,v);highZ=std::max(highZ,v);
    }
    if(std::min(highX-lowX,highZ-lowZ)<std::min(minimumWidth,minimumDepth) ||
       std::max(highX-lowX,highZ-lowZ)<std::max(minimumWidth,minimumDepth))continue;
    double x0,z0,x1,z1;
    if(!geom2::largestInscribedRectangle(polygon,nx,nz,1.,x0,z0,x1,z1))continue;
    const double w=x1-x0,d=z1-z0,area=w*d;
    if(area<=best || std::min(w,d)<std::min(minimumWidth,minimumDepth) || std::max(w,d)<std::max(minimumWidth,minimumDepth))continue;
    const auto world=[&](double x,double z){return Position{nx*x-nz*z,0,nz*x+nx*z};};
    best=area;out={world(x0,z0),world(x1,z0),world(x1,z1),world(x0,z1)};
  }
  if(out.empty() && error)*error="district union has no castle footprint after the street margin";
  return !out.empty();
}

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
    wing.brief.floorRise = b.fantasyGothic && b.landmarkLayout==LandmarkLayout::Citadel ? 4.2 : std::min(3.4, std::max(2.4, height / floors));
    wing.brief.foundation = b.foundation;
    wing.brief.shell = ringOf(r);
    wing.brief.entrance = frame.toWorld(sideMidpoint(r, doorSide).x, sideMidpoint(r, doorSide).z);
    // The city model realises every wing with the cell planner
    // (WorldCityModel, "RealisedWing"); this legacy plan only gates `solved`.
    // It takes one to three floors at 2.4-3.4 m, so a gothic citadel's keep
    // (7-8 floors at 4.2 m) failed by construction and left the castle
    // unsolved: no castle walls were composed around a castle that was built.
    if (b.fantasyGothic)
    {
      wing.planned = true;
      wing.note = "realised by the cell planner";
    }
    else
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

  if(b.fantasyGothic && b.terraceBands) {
    const unsigned count=b.terraceBands;
    const bool horizontal=gateSide==0 || gateSide==2;
    const double W=(horizontal?box.width():box.depth())-14;
    const double D=(horizontal?box.depth():box.width())-14;
    if(count<3 || count>5 || W<114 || D/count<32 || b.extraStoreys>4)
      return fail("terraced castle needs 3–5 bands, 128m width and at least 32m depth per band");
    if(b.keepPlacement==KeepPlacement::WallAttached)return fail("terraced layout uses a summit keep; attached keep is available in the single-court layout");
    if(b.dungeonLevels) return fail("terraced castle currently requires zero dungeon levels");
    if(b.landmarkLayout!=LandmarkLayout::Citadel)return fail("terraces require a citadel layout");
    plan.fantasyGothic=true;plan.landmarkLayout=LandmarkLayout::Citadel;
    const auto p=[&](double x,double z,double y=0.) {
      Position v;
      if(gateSide==0)v=frame.toWorld(box.x0+7+x,box.z0+7+z);
      else if(gateSide==2)v=frame.toWorld(box.x1-7-x,box.z1-7-z);
      else if(gateSide==1)v=frame.toWorld(box.x1-7-z,box.z0+7+x);
      else v=frame.toWorld(box.x0+7+z,box.z1-7-x);
      v.y=y;return v;
    };
    const auto rect=[&](double x0,double z0,double x1,double z1,double y=0.) {
      return ConvexRing{p(x0,z0,y),p(x1,z0,y),p(x1,z1,y),p(x0,z1,y)};
    };
    const auto solid=[&](ConvexRing ring,double bottom,double top,bool guard=false) {
      // Finished paving sits above the ground mesh, below the 5 cm dwelling floor allowance.
      plan.siteSolids.push_back({std::move(ring),b.foundation+bottom,b.foundation+top+.02,guard});
    };
    const auto wing=[&](std::string role,double x0,double z0,double x1,double z1,unsigned floors,double level) {
      CompoundWing w;w.role=role;w.brief.id=b.id*16+plan.wings.size()+1;
      w.brief.seed=rewrite::featureSeed(b.seed,4,b.id,plan.wings.size());
      w.brief.use=DwellingUse::Hall;w.brief.shell=rect(x0,z0,x1,z1);
      w.brief.foundation=b.foundation+level;w.brief.floorRise=4.2;
      w.brief.floors=floors;w.brief.availableHeight=floors*4.2;
      w.brief.entrance=p((x0+x1)*.5,z0);
      CellDwellingBrief cells;cells.id=w.brief.id;cells.seed=w.brief.seed;cells.floors=floors;
      cells.building=role=="chapel"?BuildingClass::Church:BuildingClass::Castle;
      auto grid=profileFor(cells.building).grid;grid.storeyMetres=4.2;cells.grid=grid;
      for(auto v:w.brief.shell)cells.lot.outer.push_back({geom2::toUnit(v.x),geom2::toUnit(v.z)});
      cells.frontage={0};const double area=(x1-x0)*(z1-z0);
      cells.capacity=CellDwellingBrief::Capacity{area,uint32_t(std::clamp(area/30.,1.,128.)),30.,true};
      CellDwellingSettings settings;settings.maximumFloors=0;settings.capacityCellMetres=2.4;
      settings.outerWallMetres=.8;settings.doorWidthMetres=1.6;
      settings.minimumCorridorMetres=2.4+settings.innerWallMetres;
      settings.roofPitch=1.35;settings.roofRidgeMetres=role=="keep"?12.:8.;settings.dormerChance=0;
      CellHouse house;
      w.planned=planCellDwelling(cells,settings,house,ledger,&w.note) && realiseCellDwelling(house,settings,w.plan,&w.note);
      if(w.planned)++plan.plannedWings;
      plan.wings.push_back(std::move(w));
    };
    const auto tower=[&](double x,double z,double level,double height) {
      CompoundTower t;t.height=height;
      for(unsigned k=0;k<12;++k){const double a=(k+.5)*6.283185307179586/12;t.footprint.push_back(p(x+4.5*std::cos(a),z+4.5*std::sin(a),level));}
      plan.towers.push_back(std::move(t));
    };
    const double depth=D/count,run=8.4,half=2.4;
    plan.enclosure=rect(0,0,W,D);plan.gate=p(W*.5,0);
    for(unsigned band=0;band<count;++band) {
      const double z0=band*depth,z1=(band+1)*depth,level=band*4.2;
      const double stairX=band%2?W*.70:W*.30;
      ConvexRing deck=band?ConvexRing{p(0,z0),p(stairX-half,z0),p(stairX-half,z0+run),
        p(stairX+half,z0+run),p(stairX+half,z0),p(W,z0),p(W,z1),p(0,z1)}:rect(0,z0,W,z1);
      plan.terraces.push_back({deck,b.foundation+level});solid(deck,-.6,level);
      solid(rect(0,z0,.35,z1),level,level+1.05,true);
      solid(rect(W-.35,z0,W,z1),level,level+1.05,true);
      if(band+1==count)solid(rect(0,z1-.35,W,z1),level,level+1.05,true);
      if(band) {
        const unsigned steps=24;
        plan.terraceLinks.push_back({band-1,band,steps,p(stairX,z0,b.foundation+level-4.2),p(stairX,z0+run,b.foundation+level),half*2});
        for(unsigned step=0;step<steps;++step) {
          const double a=z0+run*step/steps,c=z0+run*(step+1)/steps,h=level-4.2+(step+1)*4.2/steps;
          solid(rect(stairX-half,a,stairX+half,c),-.6,h);
          for(double side:{-1.,1.})solid(rect(stairX+side*half-.12,a,stairX+side*half+.12,c),h,h+1.05,true);
        }
        // A parapet protects the retaining edge while leaving the stair exit open.
        solid(rect(0,z0,stairX-half,z0+.35),level,level+1.05,true);
        solid(rect(stairX+half,z0,W,z0+.35),level,level+1.05,true);
      }
      // Defences follow the terrace, and each pair of wall doors meets its
      // own tower storey rather than borrowing the neighbouring band's level.
      for(double x:{5.,W-5.}) {
        tower(x,z0+6,level,21.+(band%2)*4.2);
        tower(x,z1-6,level,25.2+band*4.2);
        plan.walls.push_back({p(x,z0+6,level),p(x,z1-6,level),3.8,12.6,false});
      }
      if(band+1==count) {
        wing("keep",W*.5-16,z0+12,W*.5+16,z1-3,7+b.extraStoreys,level);
        wing("chapel",14,z0+13,28,z1-4,3+b.extraStoreys,level);
        wing("great-hall",W-31,z0+13,W-14,z1-4,4+b.extraStoreys,level);
      } else {
        wing(band?"residential-range":"guardhouse",14,z0+13,31,z1-4,3+band+b.extraStoreys,level);
        wing(band?"great-hall":"service",W-31,z0+13,W-14,z1-4,3+band+b.extraStoreys,level);
      }
      // Broad cross-courts connect alternating stair reservations. Planting
      // occupies side pockets; the route and both stair landings stay open.
      auto yard=band+1==count?rect(W*.5-6,z0+1,W*.5+6,z0+11.5,level):rect(33,z0+10,W-33,z1-2,level);plan.yards.push_back({yard,"terrace-court"});
      if(band+1<count) {
        plan.gardens.push_back({rect(W*.5-5,z0+depth*.62,W*.5+5,z1-3,level)});
        plan.fountains.push_back(p(W*.5,z0+depth*.42,level));
      }
      plan.paths.push_back({p(12,z0+10.5,level),p(W-12,z0+10.5,level),4});
      if(!band)plan.paths.push_back({p(W*.5,0,level),p(W*.5,10.5,level),4});
      if(band+1<count) {
        const double nextX=(band+1)%2?W*.70:W*.30;
        plan.paths.push_back({p(nextX,z0+10.5,level),p(nextX,z1,level),4});
      }
    }
    plan.solved=plan.plannedWings==plan.wings.size();
    if(!plan.solved)for(const auto &w:plan.wings)if(!w.planned)return fail("terrace "+w.role+": "+w.note);
    out=std::move(plan);if(error)error->clear();return true;
  }

    if(b.fantasyGothic) {
      const bool estate=b.landmarkLayout!=LandmarkLayout::Citadel;
      if(inner.width()<(estate?76:108) || inner.depth()<(estate?68:98) || b.dungeonLevels>6 || b.extraStoreys>4)
        return fail(estate ? "Gothic manor needs a 78 by 70 metre precinct and at most six dungeon levels and four extra storeys"
                           : "Gothic castle needs a 110 by 100 metre precinct and at most six dungeon levels and four extra storeys");
      plan.fantasyGothic=true;plan.landmarkLayout=b.landmarkLayout;
      plan.wallAttachedKeep=b.landmarkLayout==LandmarkLayout::Citadel &&
        (b.keepPlacement==KeepPlacement::WallAttached || (b.keepPlacement==KeepPlacement::Automatic && b.seed%3==0));
      const int keepSide=(gateSide+2)%4;
      // Keep all tower radii inside the allocated precinct.
      inner={box.x0+7,box.z0+7,box.x1-7,box.z1-7};
      // A clipped-corner circuit gives each precinct its own angled curtain.
      const double bevel=std::min(inner.width(),inner.depth())*(.10+.015*(b.seed%4));
      plan.enclosure={frame.toWorld(inner.x0+bevel,inner.z0),frame.toWorld(inner.x1-bevel,inner.z0),
        frame.toWorld(inner.x1,inner.z0+bevel),frame.toWorld(inner.x1,inner.z1-bevel),
        frame.toWorld(inner.x1-bevel*.8,inner.z1),frame.toWorld(inner.x0+bevel*1.15,inner.z1),
        frame.toWorld(inner.x0,inner.z1-bevel),frame.toWorld(inner.x0,inner.z0+bevel)};
      for(size_t edge=0;edge<plan.enclosure.size();++edge) {
        const auto a=plan.enclosure[edge],c=plan.enclosure[(edge+1)%plan.enclosure.size()];
        const double walkHeight=12.6+(((edge/2+b.seed)%3==0)?4.2:0.);
        if(edge==size_t(gateSide*2)) {
          const double span=length(a,c),t0=.5-3.5/span,t1=.5+3.5/span;
          const Position g0{a.x+(c.x-a.x)*t0,0,a.z+(c.z-a.z)*t0},g1{a.x+(c.x-a.x)*t1,0,a.z+(c.z-a.z)*t1};
          plan.walls.push_back({a,g0,3.8,walkHeight,false});plan.walls.push_back({g0,g1,3.8,walkHeight,true});
          plan.walls.push_back({g1,c,3.8,walkHeight,false});plan.gate={(g0.x+g1.x)*.5,0,(g0.z+g1.z)*.5};
        } else plan.walls.push_back({a,c,3.8,walkHeight,false});
      }
      const auto tower=[&](double x,double z,double radius,double height) {
        CompoundTower t;t.height=height;
        for(unsigned k=0;k<12;++k) {
          const double angle=(k+.5)*6.283185307179586/12;
          t.footprint.push_back(frame.toWorld(x+radius*std::cos(angle),z+radius*std::sin(angle)));
        }
        plan.towers.push_back(std::move(t));
      };
      for(auto world:plan.enclosure) {
        const auto p=frame.toLocal(world);tower(p.x,p.z,5.5,25.2+double(plan.towers.size()%3)*4.2);
      }
      for(unsigned side=0;side<4;++side)if(int(side)!=gateSide && !(plan.wallAttachedKeep && int(side)==keepSide)) {
        auto p=sideMidpoint(inner,side);tower(p.x,p.z,4.2,25.2);
      }
      const auto g=frame.toLocal(plan.gate);
      const bool horizontal=gateSide==0 || gateSide==2;
      for(double sign:{-1.,1.})tower(g.x+(horizontal?sign*8:0),g.z+(horizontal?0:sign*8),4.5,29.4);
      const double cx=(inner.x0+inner.x1)*.5,cz=(inner.z0+inner.z1)*.5;
      const double kw=std::min(34.,inner.width()*.36),kd=std::min(30.,inner.depth()*.36);
      Rect keep{cx-kw*.5,cz-kd*.5,cx+kw*.5,cz+kd*.5};
      if(plan.wallAttachedKeep) {
        // The keep replaces a curtain segment rather than overlapping a
        // solid wall inside its rooms. The mesh clips that segment away.
        if(keepSide==2){keep.z1=inner.z1+1.9;keep.z0=keep.z1-kd;}
        else if(keepSide==0){keep.z0=inner.z0-1.9;keep.z1=keep.z0+kd;}
        else if(keepSide==1){keep.x1=inner.x1+1.9;keep.x0=keep.x1-kw;}
        else {keep.x0=inner.x0-1.9;keep.x1=keep.x0+kw;}
      }
      const uint32_t keepFloors=7+uint32_t(b.seed%2)+b.extraStoreys;
      addWing("keep",keep,DwellingUse::Hall,keepFloors,keepFloors*4.2,gateSide);
      const auto requestAccess=[&](const Rect &allocation,int side,uint64_t wing,bool through) {
        const auto centre=sideMidpoint(allocation,side);
        for(const auto &wall:plan.walls) {
          if(wall.gate)continue;
          const auto a=frame.toLocal(wall.a),c=frame.toLocal(wall.b);
          const bool matching=(side==0 || side==2)?std::abs(c.z-a.z)<.01:std::abs(c.x-a.x)<.01;
          if(!matching)continue;
          const auto reference=sideMidpoint(inner,side);
          if((side==0 || side==2?std::abs(a.z-reference.z):std::abs(a.x-reference.x))>.01)continue;
          const auto add=[&](double along) {
            const auto p=(side==0 || side==2)?frame.toWorld(along,a.z):frame.toWorld(a.x,along);
            auto at=p;at.y=b.foundation+wall.height;plan.wallAccess.push_back({wing,at,3.});
          };
          if(through) {
            if(side==0 || side==2){add(allocation.x0-1.2);add(allocation.x1+1.2);}
            else {add(allocation.z0-1.2);add(allocation.z1+1.2);}
          } else add(side==0 || side==2?allocation.x0+allocation.width()*.27:allocation.z0+allocation.depth()*.27);
          break;
        }
      };
      if(plan.wallAttachedKeep) {
        requestAccess(keep,keepSide,plan.wings.back().brief.id,true);
        if(horizontal)for(double x:{keep.x0-9.,keep.x1+9.})tower(x,keepSide==2?inner.z1:inner.z0,5.5,37.8);
        else for(double z:{keep.z0-9.,keep.z1+9.})tower(keepSide==1?inner.x1:inner.x0,z,5.5,37.8);
      } else for(auto p:{Position{keep.x0-6,0,keep.z0-6},Position{keep.x1+6,0,keep.z0-6},
                  Position{keep.x1+6,0,keep.z1+6},Position{keep.x0-6,0,keep.z1+6}})
        tower(p.x,p.z,4.2,33.6+double(plan.towers.size()%3)*4.2);
      const double gap=bevel+7.,depth=10.;
      for(int side=0;side<4;++side)if(side!=gateSide && !(plan.wallAttachedKeep && side==keepSide)) {
        Rect range=side==0?Rect{inner.x0+gap,inner.z0+6.5,inner.x1-gap,inner.z0+6.5+depth}:
                   side==2?Rect{inner.x0+gap,inner.z1-6.5-depth,inner.x1-gap,inner.z1-6.5}:
                   side==1?Rect{inner.x1-6.5-depth,inner.z0+gap+depth,inner.x1-6.5,inner.z1-gap-depth}:
                           Rect{inner.x0+6.5,inner.z0+gap+depth,inner.x0+6.5+depth,inner.z1-gap-depth};
        // Side ranges stop before the rear keep on small precincts.
        if(plan.wallAttachedKeep) {
          if(keepSide==2)range.z1=std::min(range.z1,keep.z0-5.);
          if(keepSide==0)range.z0=std::max(range.z0,keep.z1+5.);
          if(keepSide==1)range.x1=std::min(range.x1,keep.x0-5.);
          if(keepSide==3)range.x0=std::max(range.x0,keep.x1+5.);
        }
        if(range.width()>=8 && range.depth()>=8) {
          const uint32_t floors=5+uint32_t((b.seed+side)%2)+b.extraStoreys;
          addWing(side==keepSide?"great-hall":"residential-range",range,DwellingUse::Hall,floors,floors*4.2,(side+2)%4);
          requestAccess(range,side,plan.wings.back().brief.id,false);
        }
      }
      // The chapel sits beside the entry, leaving the central gate axis open.
      Rect chapel=horizontal?Rect{inner.x0+20.,g.z+(gateSide==0?7:-25),inner.x0+34.,g.z+(gateSide==0?25:-7)}:
                             Rect{g.x+(gateSide==3?7:-25),inner.z0+20.,g.x+(gateSide==3?25:-7),inner.z0+34.};
      addWing("chapel",chapel,DwellingUse::Hall,2,10,horizontal?1:2);
      // Three-by-three chambers, a connected snake plus cross passages.
      // Inter-level stairs are rooted at the keep entrance, never isolated.
      const double keepX=(keep.x0+keep.x1)*.5,keepZ=(keep.z0+keep.z1)*.5;
      const double cell=std::min(8.,std::min(kw,kd)/3.),x0=keepX-1.5*cell,z0=keepZ-1.5*cell;
      const char *roles[]={"guardroom","cells","cistern","crypt","stair-hall","archive","vault","store","ritual-chamber"};
      for(uint32_t level=0;level<b.dungeonLevels;++level) {
        const uint32_t base=level*9;
        for(uint32_t row=0;row<3;++row)for(uint32_t col=0;col<3;++col) {
          const uint32_t n=row*3+col;
          plan.dungeonRooms.push_back({base+n+1,level,roles[n],ringOf({x0+col*cell,z0+row*cell,x0+(col+1)*cell,z0+(row+1)*cell}),b.foundation-4*(level+1)});
          if(col<2)plan.dungeonConnections.push_back({base+n+1,base+n+2,frame.toWorld(x0+(col+1)*cell,z0+(row+.5)*cell),false});
          if(row<2 && (col==1 || col==(level%2?0:2)))
            plan.dungeonConnections.push_back({base+n+1,base+n+4,frame.toWorld(x0+(col+.5)*cell,z0+(row+1)*cell),false});
        }
        auto stair=frame.toWorld(keepX,keepZ);stair.y=b.foundation-4*level;
        plan.dungeonConnections.push_back({level?base-4:0,base+5,stair,true});
      }
      plan.courts.push_back(ringOf(inner));
    }
  else
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
        // 3.8 m across the flats, so the square inscribed in the octagon is 5.4 m -
        // a plate the planner can actually put a room and a stair on. At 3.2 it
        // was 4.5 m and every corner tower was refused for being under the
        // five metre minimum.
        tower.footprint.push_back(frame.toWorld(corner.x + std::cos(angle) * 3.8, corner.z + std::sin(angle) * 3.8));
      }

      plan.towers.push_back(tower);

      // A tower has an inside. The square inscribed in the octagon is its
      // plate, and at three storeys the stair stands in a corner of each room
      // rather than in a core of its own - which is what a tower is.
      const double half = 3.8 * .7071;

      addWing("tower", Rect{corner.x - half, corner.z - half, corner.x + half, corner.z + half}, DwellingUse::Hall, 3, 10, 0);
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

      addWing("gatehouse", house, DwellingUse::Hall, 2, 9, horizontal ? 1 : 0);
    }

    // Keep in the corner farthest from the gate, preferring higher ground.
    const double keepW = std::min(28., inner.width() * .4), keepD = std::min(24., inner.depth() * .4);
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

    const uint32_t keepFloors=keepW*keepD>=240 ? 3 : 2;
    addWing("keep", keep, DwellingUse::Hall, keepFloors, keepFloors*3.4, gateSide == 0 || gateSide == 2 ? (keep.z0 > inner.z0 + inner.depth() * .5 ? 0 : 2) : (keep.x0 > inner.x0 + inner.width() * .5 ? 3 : 1));

    // Hall range along the long wall away from the keep; store range opposite.
    const bool keepLeft = (keep.x0 + keep.x1) * .5 < (inner.x0 + inner.x1) * .5;
    const bool keepSouth = (keep.z0 + keep.z1) * .5 < (inner.z0 + inner.z1) * .5;
    // Ranges lean against the inside face of the curtain. The small clearance
    // is construction tolerance, not a lane behind the building.
    constexpr double wallGap = .15;
    Rect hall = keepSouth ? Rect{inner.x0 + 4, inner.z1 - wallGap - 8, inner.x1 - 4, inner.z1 - wallGap} : Rect{inner.x0 + 4, inner.z0 + wallGap, inner.x1 - 4, inner.z0 + wallGap + 8};

    if (gateSide == (keepSouth ? 2 : 0))
    {
      hall = keepLeft ? Rect{inner.x1 - wallGap - 8, inner.z0 + 4, inner.x1 - wallGap, inner.z1 - 4} : Rect{inner.x0 + wallGap, inner.z0 + 4, inner.x0 + wallGap + 8, inner.z1 - 4};
    }

    if (hall.width() >= 12 && hall.depth() >= 6)
    {
      addWing("hall", hall, DwellingUse::Hall, 1, 6, keepSouth ? 0 : 2);
    }

    Rect store = keepLeft ? Rect{inner.x1 - wallGap - 6, keepSouth ? inner.z0 + wallGap : inner.z1 - wallGap - 12, inner.x1 - wallGap, keepSouth ? inner.z0 + wallGap + 12 : inner.z1 - wallGap}
                          : Rect{inner.x0 + wallGap, keepSouth ? inner.z0 + wallGap : inner.z1 - wallGap - 12, inner.x0 + wallGap + 6, keepSouth ? inner.z0 + wallGap + 12 : inner.z1 - wallGap};

    if (store.width() >= 5 && store.depth() >= 6)
    {
      addWing("stables", store, DwellingUse::Warehouse, 1, 5, keepLeft ? 3 : 1);
    }

    plan.courts.push_back(ringOf(inner)); // Bailey: the enclosure minus wings (accounting only).
    break;
  }
  case CompoundKind::Manor: {
    // An estate is land, not the envelope of one enormous service range.
    // Keep a compact court beside the entrance; the remainder is estate land.
    const double width=std::min(inner.width(),36.), depth=std::min(inner.depth(),36.);
    if(gateSide==1) inner.x0=inner.x1-width;
    else if(gateSide==3) inner.x1=inner.x0+width;
    else {const double cx=(inner.x0+inner.x1)*.5;inner.x0=cx-width*.5;inner.x1=cx+width*.5;}
    if(gateSide==2) inner.z0=inner.z1-depth;
    else if(gateSide==0) inner.z1=inner.z0+depth;
    else {const double cz=(inner.z0+inner.z1)*.5;inner.z0=cz-depth*.5;inner.z1=cz+depth*.5;}

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
  case CompoundKind::Monastery: {
    if (inner.width()<36 || inner.depth()<34)
      return fail("monastery precinct needs at least 36 by 34 metres inside its boundary");
    // Five separately buildable wings frame an actual unoccupied cloister
    // court. Two-metre passages retain access at every corner and keep roofs
    // apart; a roofed arcade can later consume the same court boundary.
    const double x0=inner.x0+2,x1=inner.x1-2,z0=inner.z0+2,z1=inner.z1-2;
    const double wing=8, gap=2;
    const double churchDepth=12;
    addWing("church",{x0,z1-churchDepth,x1,z1},DwellingUse::Hall,1,9,0);
    addWing("dormitory",{x0,z0+wing+gap,x0+wing,z1-churchDepth-gap},DwellingUse::Lodging,2,6,1);
    addWing("chapter-house",{x1-wing,z0+wing+gap,x1,z1-churchDepth-gap},DwellingUse::Hall,1,4,3);
    addWing("refectory",{x0,z0,x1-wing-gap,z0+wing},DwellingUse::Hall,1,4,2);
    addWing("kitchen",{x1-wing,z0,x1,z0+wing},DwellingUse::Warehouse,1,4,2);
    plan.courts.push_back(ringOf({x0+wing+gap,z0+wing+gap,x1-wing-gap,z1-churchDepth-gap}));
    plan.gate=b.approach;
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

  if(b.fantasyGothic) {
    // Normalised estate coordinates always run from the entrance to the back.
    const auto uv=[&](double u,double v) {
      if(gateSide==1)return Position{inner.x1-v*inner.width(),0,inner.z0+u*inner.depth()};
      if(gateSide==2)return Position{inner.x1-u*inner.width(),0,inner.z1-v*inner.depth()};
      if(gateSide==3)return Position{inner.x0+v*inner.width(),0,inner.z1-u*inner.depth()};
      return Position{inner.x0+u*inner.width(),0,inner.z0+v*inner.depth()};
    };
    const auto rect=[&](double u0,double v0,double u1,double v1) {
      auto a=uv(u0,v0),c=uv(u1,v1);return Rect{std::min(a.x,c.x),std::min(a.z,c.z),std::max(a.x,c.x),std::max(a.z,c.z)};
    };
    const auto point=[&](double u,double v){auto p=uv(u,v);return frame.toWorld(p.x,p.z);};
    if(b.landmarkLayout!=LandmarkLayout::Citadel) {
      plan.wings.clear();plan.towers.clear();plan.walls.clear();plan.plannedWings=0;
      plan.dungeonRooms.clear();plan.dungeonConnections.clear();plan.wallAccess.clear();plan.kind=CompoundKind::Manor;
      const bool palace=b.landmarkLayout==LandmarkLayout::Palace;
      wallRing(inner,.6,2.2,gateSide,9);
      addWing(palace?"royal-apartments":"manor-hall",rect(.22,.70,.78,.86),DwellingUse::Hall,palace?4:3,palace?16:12,gateSide);
      addWing(palace?"library-gallery":"service-wing",rect(.22,.42,.34,.70),DwellingUse::Hall,palace?3:2,palace?12:8,(gateSide+1)%4);
      addWing(palace?"banquet-gallery":"guest-wing",rect(.66,.42,.78,.70),DwellingUse::Hall,palace?3:2,palace?12:8,(gateSide+3)%4);
      if(palace) {
        addWing("reception-pavilion",rect(.13,.29,.34,.42),DwellingUse::Hall,4,16,gateSide);
        addWing("ballroom-pavilion",rect(.66,.29,.87,.42),DwellingUse::Hall,4,16,gateSide);
      }
      for(double u:{.12,.30,.70,.88})for(double v:{.08,.20})
        plan.gardens.push_back({ringOf(rect(u-.06,v-.045,u+.06,v+.045))});
      for(double u:{.42,.58})plan.gardens.push_back({ringOf(rect(u-.045,.46,u+.045,.61))});
      plan.fountains.push_back(point(.5,.22));plan.fountains.push_back(point(.5,.55));
      plan.paths.push_back({point(.5,0),point(.5,.70),10});
      plan.paths.push_back({point(.08,.34),point(.92,.34),5});
      plan.paths.push_back({point(.08,.14),point(.92,.14),3});
    } else {
      // Front garden pockets avoid the chapel, gate axis and keep approaches.
      plan.gardens.push_back({ringOf(rect(.30,.08,.42,.24))});
      plan.gardens.push_back({ringOf(rect(.58,.08,.74,.24))});
      plan.fountains.push_back(point(.50,.20));
      plan.paths.push_back({point(.50,0),point(.50,.35),10});
      plan.paths.push_back({point(.20,.30),point(.80,.30),5});
      plan.paths.push_back({point(.20,.30),point(.20,.72),4});
      plan.paths.push_back({point(.80,.30),point(.80,.72),4});
    }
  }

  if(b.fantasyGothic) {
    // SAT clearance works on the convex allocation rings, including rotated
    // precincts. Landscape and annex reservations never claim occupied land.
    const auto overlap=[](const ConvexRing &a,const ConvexRing &c,double margin) {
      for(const auto *ring:{&a,&c})for(size_t i=0;i<ring->size();++i) {
        const auto p=(*ring)[i],q=(*ring)[(i+1)%ring->size()];
        const double dx=q.x-p.x,dz=q.z-p.z,len=std::hypot(dx,dz);if(len<1e-8)continue;
        const double nx=-dz/len,nz=dx/len;double al=1e100,ah=-1e100,cl=1e100,ch=-1e100;
        for(auto v:a){const double t=v.x*nx+v.z*nz;al=std::min(al,t);ah=std::max(ah,t);}
        for(auto v:c){const double t=v.x*nx+v.z*nz;cl=std::min(cl,t);ch=std::max(ch,t);}
        if(ah+margin<=cl || ch+margin<=al)return false;
      }return true;
    };
    const auto fits=[&](const ConvexRing &ring,double clearance) {
      for(auto p:ring)for(size_t i=0;i<plan.enclosure.size();++i) {
        auto a=plan.enclosure[i],c=plan.enclosure[(i+1)%plan.enclosure.size()];
        if((c.x-a.x)*(p.z-a.z)-(c.z-a.z)*(p.x-a.x)<clearance*std::hypot(c.x-a.x,c.z-a.z))return false;
      }
      for(const auto &wing:plan.wings)if(overlap(ring,wing.brief.shell,clearance))return false;
      for(const auto &tower:plan.towers)if(overlap(ring,tower.footprint,clearance))return false;
      return true;
    };
    // Remove old decorative pockets if a layout variation has taken their site.
    plan.gardens.erase(std::remove_if(plan.gardens.begin(),plan.gardens.end(),[&](const auto &g){return !fits(g.footprint,1.5);}),plan.gardens.end());
    if(b.landmarkLayout==LandmarkLayout::Citadel && inner.width()*inner.depth()>12000) {
      // Small service ranges use remaining perimeter pockets, while the main
      // court remains reserved for circulation and open ground.
      unsigned count=0;
      for(const Rect &r:{Rect{inner.x0+20,inner.z1-28,inner.x0+33,inner.z1-15},
                         Rect{inner.x1-33,inner.z0+20,inner.x1-20,inner.z0+34},
                         Rect{inner.x1-33,inner.z1-28,inner.x1-20,inner.z1-15}}) {
        const auto ring=ringOf(r);bool free=fits(ring,3.);
        for(const auto &g:plan.gardens)free&=!overlap(ring,g.footprint,2.);
        for(const auto &path:plan.paths) {
          const double dx=path.b.x-path.a.x,dz=path.b.z-path.a.z,len=std::hypot(dx,dz);if(len<.1)continue;
          const double nx=-dz/len*(path.width*.5+2),nz=dx/len*(path.width*.5+2);
          free&=!overlap(ring,{{path.a.x-nx,0,path.a.z-nz},{path.b.x-nx,0,path.b.z-nz},{path.b.x+nx,0,path.b.z+nz},{path.a.x+nx,0,path.a.z+nz}},0);
        }
        if(free){const uint32_t floors=2+count%2+b.extraStoreys;addWing(count%2?"kitchens":"barracks",r,DwellingUse::Hall,floors,4.2*floors,gateSide);++count;}
      }
    }
    // Maximal free rectangles on a four-metre occupancy board. Infrastructure
    // and buildings are reserved first; yards expand only into usable space.
    const double step=4.;const unsigned nx=std::min(128u,unsigned(inner.width()/step)),nz=std::min(128u,unsigned(inner.depth()/step));
    std::vector<bool> free(nx*nz,false);
    for(unsigned z=0;z<nz;++z)for(unsigned x=0;x<nx;++x) {
      const auto ring=ringOf({inner.x0+x*step,inner.z0+z*step,inner.x0+(x+1)*step,inner.z0+(z+1)*step});
      bool ok=fits(ring,2.5);
      for(const auto &g:plan.gardens)ok&=!overlap(ring,g.footprint,1.);
      for(const auto &f:plan.fountains) {ConvexRing basin;for(unsigned k=0;k<12;++k){const double a=k*6.28318530718/12;basin.push_back({f.x+4*std::cos(a),0,f.z+4*std::sin(a)});}ok&=!overlap(ring,basin,0);}
      for(const auto &path:plan.paths) {
        const double dx=path.b.x-path.a.x,dz=path.b.z-path.a.z,len=std::hypot(dx,dz);if(len<.1)continue;
        const double ex=-dz/len*(path.width*.5+1),ez=dx/len*(path.width*.5+1);
        ok&=!overlap(ring,{{path.a.x-ex,0,path.a.z-ez},{path.b.x-ex,0,path.b.z-ez},{path.b.x+ex,0,path.b.z+ez},{path.a.x+ex,0,path.a.z+ez}},0);
      }
      free[z*nx+x]=ok;
    }
    for(unsigned yard=0;yard<4;++yard) {
      unsigned best=0,bx=0,bz=0,bw=0,bh=0;
      for(unsigned z=0;z<nz;++z)for(unsigned x=0;x<nx;++x)if(free[z*nx+x]) {
        unsigned width=nx-x;
        for(unsigned high=z;high<nz;++high) {
          unsigned run=0;while(run<width && free[high*nx+x+run])++run;width=run;if(width<3)break;
          const unsigned height=high-z+1;if(height>=3 && width*height>best){best=width*height;bx=x;bz=z;bw=width;bh=height;}
        }
      }
      if(best<12)break;
      const Rect yardRect{inner.x0+bx*step+.5,inner.z0+bz*step+.5,inner.x0+(bx+bw)*step-.5,inner.z0+(bz+bh)*step-.5};
      plan.yards.push_back({ringOf(yardRect),yard==0?"great-yard":"garden-yard"});
      if(best>=24) {
        const double inset=3.;const Rect bed{yardRect.x0+inset,yardRect.z0+inset,yardRect.x0+yardRect.width()*.45,yardRect.z1-inset};
        if(bed.width()>=4 && bed.depth()>=6)plan.gardens.push_back({ringOf(bed)});
      }
      for(unsigned z=bz;z<bz+bh;++z)for(unsigned x=bx;x<bx+bw;++x)free[z*nx+x]=false;
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
