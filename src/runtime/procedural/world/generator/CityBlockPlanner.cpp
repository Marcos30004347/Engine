#include "CityBlockPlanner.hpp"

#include "PlazaPlanner.hpp"
#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {
namespace {

using geom2::Polygon;

Polygon toPolygon(const std::vector<Position> &ring)
{
  Polygon p;
  for (const auto &q : ring)
  {
    p.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
  }

  if (!geom2::counterClockwise(p.outer))
  {
    geom2::reverse(p.outer);
  }

  return p;
}

std::vector<Position> toRing(const geom2::Ring &ring)
{
  std::vector<Position> out;
  out.reserve(ring.size());

  for (const auto &p : ring)
  {
    out.push_back({geom2::toMetres(p.x), 0, geom2::toMetres(p.y)});
  }

  return out;
}

// The strip of ground within `depth` of one edge, clipped to the block.
//
// Built as a quad off the edge rather than by offsetting the whole ring,
// because a frontage belongs to ONE edge: offsetting inward gives a ring that
// belongs to all of them at once and cannot be cut into plots that face
// anything in particular.
bool strip(const Position &a, const Position &b, const double depth, const Polygon &within, std::vector<Polygon> &out, std::string *error)
{
  const double dx = b.x - a.x, dz = b.z - a.z;
  const double length = std::hypot(dx, dz);

  if (length < 1e-6 || depth <= 0)
  {
    return true;
  }

  // Inward is to the left of a counter clockwise edge.
  const double nx = -dz / length * depth, nz = dx / length * depth;
  // Run the quad past both ends so neighbouring frontages overlap at the
  // corner rather than leaving a wedge nobody owns; the overlap is resolved
  // by whichever edge claims it first.
  const double ox = dx / length * depth, oz = dz / length * depth;

  Polygon quad;
  for (const auto &p : std::vector<std::pair<double, double>>{
           {a.x - ox, a.z - oz}, {b.x + ox, b.z + oz}, {b.x + ox + nx, b.z + oz + nz}, {a.x - ox + nx, a.z - oz + nz}})
  {
    quad.outer.push_back({geom2::toUnit(p.first), geom2::toUnit(p.second)});
  }

  if (!geom2::counterClockwise(quad.outer))
  {
    geom2::reverse(quad.outer);
  }

  return geom2::intersectionOf({geom2::toOperand(quad), geom2::toOperand(within)}, out, error);
}

} // namespace

const char *nameOf(const CityBlockLotRole role)
{
  static constexpr const char *names[] = {"dwelling", "shop-house", "workshop", "passage", "pub", "inn", "bakehouse", "smithy", "cottage"};
  static_assert(sizeof(names) / sizeof(names[0]) == size_t(CityBlockLotRole::Cottage) + 1,
                "every role needs a name: reading past this table is a null char* and a crash in strlen");

  return names[size_t(role)];
}

bool planCityBlock(const CityBlockPlanInput &in, CityBlockPlan &out, std::string *error)
{
  auto fail = [&](const std::string &why) {
    if (error != nullptr)
    {
      *error = why;
    }

    return false;
  };

  if (in.boundary.size() < 3)
  {
    return fail("a block needs a boundary");
  }

  if (in.streetEdges.size() != in.boundary.size())
  {
    return fail("a block needs to know which of its edges face a street");
  }

  if (!std::isfinite(in.sidewalkMetres) || in.sidewalkMetres < 0 ||
      !std::isfinite(in.corridorMetres) || in.corridorMetres <= 0 ||
      !std::isfinite(in.lotDepthMetres) || in.lotDepthMetres <= 0 ||
      !std::isfinite(in.lotFrontageMetres) || in.lotFrontageMetres <= 0 ||
      !std::isfinite(in.minimumLotWidthMetres) || in.minimumLotWidthMetres <= 0 ||
      !std::isfinite(in.minimumLotMetres2) || in.minimumLotMetres2 <= 0 ||
      !std::isfinite(in.maximumCourtMetres2) || in.maximumCourtMetres2 <= 0)
    return fail("block dimensions must be finite and positive");
  geom2::Ring inputRing;
  for (const auto &p : in.boundary) {
    if(!std::isfinite(p.x) || !std::isfinite(p.z)) return fail("non-finite block vertex");
    inputRing.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
  }
  if(!geom2::counterClockwise(inputRing)) return fail("block boundary must be counter clockwise");
  if(!geom2::polygonFault(toPolygon(in.boundary)).empty()) return fail("invalid block boundary");

  CityBlockPlan plan;
  plan.id = in.id;

  const Polygon block = toPolygon(in.boundary);
  const double blockArea = std::abs(geom2::areaMetres(block));

  if (blockArea < in.minimumLotMetres2)
  {
    return fail("block is smaller than one lot");
  }

  // --- a public square ---------------------------------------------------
  //
  // Paved to its edges, with stalls round the inside of it and nothing in the
  // middle but the space itself. A market square earns its keep by being
  // empty; the buildings that face it belong to the blocks around it, not to
  // this one.
  if (in.plaza)
  {
    plan.paving.push_back(in.boundary);

    // What stands on it is the plaza planner's: the arcade and its gates,
    // the dais, the monument, and the shambles and the hall inside the
    // arcade. A street edge of the block is where a gate goes.
    PlazaBrief brief;
    brief.id = in.id;
    brief.seed = in.seed;
    brief.ring = in.boundary;
    brief.shamblesFrontageMetres = in.shamblesFrontageMetres;
    brief.shamblesDepthMetres = in.shamblesDepthMetres;
    brief.marketHallWidthMetres = in.marketHallWidthMetres;
    brief.marketHallDepthMetres = in.marketHallDepthMetres;
    brief.marketHall = in.marketHall;

    for (size_t i = 0; i < in.boundary.size(); ++i)
    {
      if (!in.streetEdges[i])
      {
        continue;
      }

      const Position a = in.boundary[i];
      const Position b = in.boundary[(i + 1) % in.boundary.size()];
      const double run = std::hypot(b.x - a.x, b.z - a.z);

      if (run < 9.)
      {
        continue;
      }

      brief.entries.push_back({{(a.x + b.x) * .5, 0, (a.z + b.z) * .5}, {-(b.z - a.z) / run, 0, (b.x - a.x) / run}, 5.});
    }

    Ledger ledger;
    std::string why;

    if (!PlazaPlanner::plan(brief, plan.plaza, ledger, &why))
    {
      return fail(why);
    }

    plan.stalls = plan.plaza.stalls;
    plan.halls = plan.plaza.halls;
    plan.notes = plan.plaza.notes;
    out = std::move(plan);

    return true;
  }

  // --- the sidewalk ------------------------------------------------------
  //
  // Paved ground between the carriageway and the plots, on the street edges
  // only. A block's back edge abuts another block's back edge and wants no
  // pavement; putting one there is what made every block read as an island.
  std::vector<geom2::Operand> paved;

  for (size_t i = 0; i < in.boundary.size(); ++i)
  {
    if (!in.streetEdges[i])
    {
      continue;
    }

    std::vector<Polygon> walk;
    if (!strip(in.boundary[i], in.boundary[(i + 1) % in.boundary.size()], in.sidewalkMetres, block, walk, error))
    {
      return false;
    }

    for (auto &piece : walk)
    {
      if (std::abs(geom2::areaMetres(piece)) < .5)
      {
        continue;
      }

      paved.push_back(geom2::toOperand(piece));
      plan.sidewalks.push_back(toRing(piece.outer));
    }
  }

  // What is left to build on.
  std::vector<Polygon> core{block};

  if (!paved.empty())
  {
    std::vector<Polygon> cut;
    if (!geom2::differenceOf(geom2::toOperand(block), paved, cut, error))
    {
      return false;
    }

    if (cut.empty()) return fail("sidewalks consume the block");
    core = std::move(cut);
  }

  const Polygon &buildable = *std::max_element(core.begin(), core.end(), [](const Polygon &a, const Polygon &b) {
    return std::abs(geom2::areaMetres(a)) < std::abs(geom2::areaMetres(b));
  });

  std::vector<CityBlockPlan> interiorPlans;

  // Reserve the common and its approaches before allocating private ground.
  // Two opposing entrances keep a large block permeable; a small block still
  // uses all its depth for houses. Offsetting follows skewed block boundaries.
  if (!in.crofts && in.commonCourt)
  {
    std::vector<Polygon> courts;
    if (!geom2::offsetPolygon(buildable, -in.lotDepthMetres, {}, courts, error)) return false;
    for (const auto &court : courts)
    {
      if (!court.holes.empty() || std::abs(geom2::areaMetres(court)) < in.gardenMinimumMetres2) continue;
      Position centre{};
      for (const auto &p : court.outer) { centre.x += geom2::toMetres(p.x)/court.outer.size(); centre.z += geom2::toMetres(p.y)/court.outer.size(); }
      if(!geom2::contains(court,{geom2::toUnit(centre.x),geom2::toUnit(centre.z)}))continue;
      // A deep block gets another residential court behind an access lane,
      // rather than turning its entire interior into one enormous square.
      CityBlockPlan inner;
      bool infill=false;
      if(std::abs(geom2::areaMetres(court))>in.maximumCourtMetres2+4*in.minimumLotMetres2) {
        CityBlockPlanInput next=in;
        next.boundary=toRing(court.outer);
        next.streetEdges.assign(next.boundary.size(),true);
        next.streetAxis.clear();
        next.sidewalkMetres=std::max(1.6,in.corridorMetres);
        next.id=in.id*31+interiorPlans.size()+1;
        std::string innerError;
        infill=planCityBlock(next,inner,&innerError) && !inner.lots.empty();
      }
      if(infill) {
        for(const auto &walk:inner.sidewalks)plan.corridors.push_back(walk);
        for(const auto &way:inner.corridors)plan.corridors.push_back(way);
        for(const auto &square:inner.paving)plan.paving.push_back(square);
        for(const auto &garden:inner.gardens)plan.gardens.push_back(garden);
        interiorPlans.push_back(std::move(inner));
        plan.notes.push_back("deep interior subdivided into lane-fronted housing and a smaller common");
      } else plan.paving.push_back(toRing(court.outer));
      // Reserve the whole inner plan from the outer frontage cutters and
      // stop their entrance corridors at the connected inner perimeter lane.
      paved.push_back(geom2::toOperand(court));
      std::vector<size_t> entrances;
      for (size_t i=0;i<in.boundary.size();++i) if(in.streetEdges[i]) entrances.push_back(i);
      std::stable_sort(entrances.begin(),entrances.end(),[&](size_t i,size_t j) {
        const auto length=[&](size_t k) { const auto a=in.boundary[k],b=in.boundary[(k+1)%in.boundary.size()]; return std::hypot(b.x-a.x,b.z-a.z); };
        return length(i)>length(j);
      });
      size_t first=SIZE_MAX;
      for (const size_t edge : entrances)
      {
        const auto a=in.boundary[edge],b=in.boundary[(edge+1)%in.boundary.size()];
        if(first!=SIZE_MAX) {
          const auto c=in.boundary[first],d=in.boundary[(first+1)%in.boundary.size()];
          if((b.x-a.x)*(d.x-c.x)+(b.z-a.z)*(d.z-c.z)>=0) continue;
        }
        const Position start{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
        const double dx=centre.x-start.x,dz=centre.z-start.z,len=std::hypot(dx,dz);
        if(len<1.) continue;
        const double nx=-dz/len*in.corridorMetres*.5,nz=dx/len*in.corridorMetres*.5;
        Polygon way=toPolygon({{start.x+nx,0,start.z+nz},{start.x-nx,0,start.z-nz},
                               {centre.x-nx,0,centre.z-nz},{centre.x+nx,0,centre.z+nz}});
        std::vector<Polygon> clipped;
        if(!geom2::intersectionOf({geom2::toOperand(way),geom2::toOperand(block)},clipped,error)) return false;
        for(const auto &piece:clipped) {
          std::vector<Polygon> clear;
          if(!geom2::differenceOf(geom2::toOperand(piece),paved,clear,error)) return false;
          for(const auto &q:clear) { plan.corridors.push_back(toRing(q.outer)); paved.push_back(geom2::toOperand(q)); }
        }
        if(first!=SIZE_MAX) break;
        first=edge;
      }
    }
  }

  // --- the frontage rows -------------------------------------------------
  //
  // One row per street edge, each cut into plots across its own frontage, so
  // every plot faces the street it was cut from and the plots along one edge
  // share party lines. Depth is bounded by what the block has: a shallow
  // block is all frontage and has no middle, which is correct and is why the
  // garden below can come out empty.
  std::vector<Polygon> taken;
  uint32_t edgeIndex = 0;

  for (size_t i = 0; i < in.boundary.size(); ++i)
  {
    if (!in.streetEdges[i])
    {
      continue;
    }

    const Position a = in.boundary[i];
    const Position b = in.boundary[(i + 1) % in.boundary.size()];
    double dx = b.x - a.x, dz = b.z - a.z;
    double run = std::hypot(dx, dz);

    if (run < in.lotFrontageMetres * .6)
    {
      ++edgeIndex;
      continue;
    }

    // Square to the road.
    //
    // The cuts used to run across the block EDGE, and a block edge sits a few
    // degrees off the street it faces - enough that every lot met the road at
    // a slightly different angle and the frontage read as a row of wedges
    // rather than a terrace. Given the street's own direction the cuts are
    // square to it, every plot on that street shares one axis, and the row
    // packs.
    if (i < in.streetAxis.size())
    {
      const Position &axis = in.streetAxis[i];
      const double length = std::hypot(axis.x, axis.z);

      if (length > .5)
      {
        // Kept pointing the way the edge runs, so the row is still walked
        // from a to b.
        const double sign = (axis.x * dx + axis.z * dz) < 0 ? -1. : 1.;
        dx = axis.x / length * run * sign;
        dz = axis.z / length * run * sign;
        run = std::hypot(dx, dz);
      }
    }

    std::vector<Polygon> row;
    if (!strip(a, b, in.sidewalkMetres + in.lotDepthMetres, block, row, error))
    {
      return false;
    }

    // Never over ground another frontage already took, nor over the paving.
    std::vector<geom2::Operand> already = paved;
    for (const auto &p : taken)
    {
      already.push_back(geom2::toOperand(p));
    }

    std::vector<Polygon> free;
    for (const auto &piece : row)
    {
      std::vector<Polygon> clear;
      if (already.empty())
      {
        clear = {piece};
      }
      else if (!geom2::differenceOf(geom2::toOperand(piece), already, clear, error))
      {
        return false;
      }

      for (auto &q : clear)
      {
        if (std::abs(geom2::areaMetres(q)) >= in.minimumLotMetres2 * .5)
        {
          free.push_back(std::move(q));
        }
      }
    }

    // Share the run out into whole plots, as near the wanted frontage as the
    // edge allows: a row of equal plots reads as a terrace, a row of leftovers
    // reads as a mistake.
    const uint32_t count = uint32_t(std::max(1., std::round(run / in.lotFrontageMetres)));
    const double width = run / double(count);
    const uint32_t first = uint32_t(plan.lots.size());

    for (uint32_t k = 0; k < count; ++k)
    {
      double t0 = double(k) / double(count), t1 = double(k + 1) / double(count);

      // The end plots reach past the end of their own edge.
      //
      // The row is cut a little beyond both ends of its edge, so that two
      // rows meeting at a corner overlap and the first one to claim it keeps
      // it. The CUTTERS only spanned the edge itself, though, so that
      // overhang belonged to no plot at all and every corner of the block
      // came out as a hole with pavement running round it. The first and last
      // plot of a row run out to where the row does.
      const double over = (in.sidewalkMetres + in.lotDepthMetres) / run;
      if (k == 0) t0 -= over;
      if (k + 1 == count) t1 += over;

      // A cutter across the row, from the street edge inward.
      const double cx0 = a.x + dx * t0, cz0 = a.z + dz * t0;
      const double cx1 = a.x + dx * t1, cz1 = a.z + dz * t1;
      const double reach = in.sidewalkMetres + in.lotDepthMetres + 2.;
      const double nx = -dz / run * reach, nz = dx / run * reach;

      Polygon cutter;
      for (const auto &p : std::vector<std::pair<double, double>>{
               {cx0, cz0}, {cx1, cz1}, {cx1 + nx, cz1 + nz}, {cx0 + nx, cz0 + nz}})
      {
        cutter.outer.push_back({geom2::toUnit(p.first), geom2::toUnit(p.second)});
      }

      // On a convex four-sided block, interpolate between opposite edges.
      // This is a bilinear grid: adjacent parcels share the exact same cut,
      // including on a trapezoid, instead of wasting diagonal corner wedges.
      bool convexQuad=in.boundary.size()==4;
      for(size_t corner=0;corner<in.boundary.size() && convexQuad;++corner) {
        const auto p=in.boundary[corner],q=in.boundary[(corner+1)%4],r=in.boundary[(corner+2)%4];
        convexQuad=(q.x-p.x)*(r.z-q.z)-(q.z-p.z)*(r.x-q.x)>0;
      }
      if(convexQuad && in.conformingQuadLots) {
        const auto c=in.boundary[(i+2)%4],d=in.boundary[(i+3)%4];
        const double ux=(b.x-a.x)/run,uz=(b.z-a.z)/run;
        const double depthA=(d.x-a.x)*(-uz)+(d.z-a.z)*ux;
        const double depthB=(c.x-b.x)*(-uz)+(c.z-b.z)*ux;
        if(std::min(depthA,depthB)>reach) {
          const double f=std::min(1.,reach/std::min(depthA,depthB));
          const Position backA{a.x+(d.x-a.x)*f,0,a.z+(d.z-a.z)*f};
          const Position backB{b.x+(c.x-b.x)*f,0,b.z+(c.z-b.z)*f};
          const auto lerp=[](Position p,Position q,double t){return Position{p.x+(q.x-p.x)*t,0,p.z+(q.z-p.z)*t};};
          cutter=toPolygon({lerp(a,b,t0),lerp(a,b,t1),lerp(backA,backB,t1),lerp(backA,backB,t0)});
        }
      }
      if (!geom2::counterClockwise(cutter.outer))
      {
        geom2::reverse(cutter.outer);
      }

      for (const auto &piece : free)
      {
        std::vector<Polygon> lot;
        if (!geom2::intersectionOf({geom2::toOperand(cutter), geom2::toOperand(piece)}, lot, error))
        {
          return false;
        }

        for (auto &q : lot)
        {
          const double area = std::abs(geom2::areaMetres(q));
          if (area < .1 || !q.holes.empty()) continue;

          CityBlockLot made;
          made.boundary = toRing(q.outer);
          made.area = area;
          made.frontageEdge = edgeIndex;
          made.corner = k == 0 || k + 1 == count;
          plan.lots.push_back(std::move(made));

        }
      }
    }

    // Repair clipped slivers before admitting houses. Only unions sharing a
    // real boundary are accepted; a public passage cannot be merged away.
    const auto usable=[&](const CityBlockLot &lot) {
      if(lot.area<in.minimumLotMetres2)return false;
      std::vector<Polygon> inset;
      return geom2::offsetPolygon(toPolygon(lot.boundary),-in.minimumLotWidthMetres*.5,{},inset,nullptr) &&
             std::any_of(inset.begin(),inset.end(),[](const Polygon &p){return geom2::areaMetres(p)>.5;});
    };
    for(size_t k=first;k<plan.lots.size();) {
      if(usable(plan.lots[k])){++k;continue;}
      size_t neighbour=SIZE_MAX;Polygon joined;double best=1e100;
      for(size_t j=first;j<plan.lots.size();++j)if(j!=k) {
        std::vector<Polygon> merged;
        if(!geom2::unionOf({geom2::toOperand(toPolygon(plan.lots[k].boundary)),
                           geom2::toOperand(toPolygon(plan.lots[j].boundary))},merged,nullptr) ||
           merged.size()!=1 || !merged[0].holes.empty())continue;
        const double area=std::abs(geom2::areaMetres(merged[0]));
        if(std::abs(area-plan.lots[k].area-plan.lots[j].area)>.01)continue;
        if(area<best){best=area;neighbour=j;joined=std::move(merged[0]);}
      }
      if(neighbour!=SIZE_MAX) {
        plan.lots[neighbour].boundary=toRing(joined.outer);
        plan.lots[neighbour].area=best;
        plan.lots[neighbour].corner|=plan.lots[k].corner;
        plan.lots.erase(plan.lots.begin()+k);
        k=first;
        plan.notes.push_back("undersized frontage merged with its neighbour");
      } else {
        plan.gardens.push_back(plan.lots[k].boundary);
        paved.push_back(geom2::toOperand(toPolygon(plan.lots[k].boundary)));
        plan.lots.erase(plan.lots.begin()+k);
        plan.notes.push_back("unbuildable frontage retained as shared ground");
      }
    }
    for(size_t k=first;k<plan.lots.size();++k)taken.push_back(toPolygon(plan.lots[k].boundary));

    // Neighbours along this row, so a terrace knows who it meets.
    for (uint32_t k = first; k < plan.lots.size(); ++k)
    {
      if (k > first)
      {
        plan.lots[k].before = k - 1;
      }

      if (k + 1 < plan.lots.size())
      {
        plan.lots[k].after = k + 1;
      }
    }

    ++edgeIndex;
  }

  if (plan.lots.empty())
  {
    return fail("block took no lots");
  }

  // --- crofts ------------------------------------------------------------
  //
  // A suburb is not a thinner town. Its households keep a pig and a few hens,
  // grow most of what they eat, and the house is the small built end of a
  // long strip of ground running back from the road - toft, yard, pen, then
  // garden. Allocating it as a row of terraced plots, which is what it got,
  // produced a suburb of town houses standing shoulder to shoulder with
  // nowhere to keep anything.
  //
  // The strip is already cut: it is the frontage row, made deeper and wider.
  // What this adds is the division of it, so the parts exist as ground that
  // something can be put on rather than as one undifferentiated plot.
  if (in.crofts)
  {
    for (auto &lot : plan.lots)
    {
      lot.role = CityBlockLotRole::Cottage;

      // The strip's own axis: from its street end backward. Taken from the
      // frontage edge so every croft on a row divides the same way.
      Position front{}, back{};
      double best = 1e30, worst = -1e30;

      for (const auto &p : lot.boundary)
      {
        double near = 1e30;
        for (size_t i = 0; i < in.boundary.size(); ++i)
        {
          if (!in.streetEdges[i])
          {
            continue;
          }

          const Position &a = in.boundary[i];
          const Position &b = in.boundary[(i + 1) % in.boundary.size()];
          const double dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
          const double t = len2 > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / len2, 0., 1.) : 0.;
          near = std::min(near, std::hypot(p.x - a.x - dx * t, p.z - a.z - dz * t));
        }

        if (near < best) { best = near; front = p; }
        if (near > worst) { worst = near; back = p; }
      }

      const double dx = back.x - front.x, dz = back.z - front.z;
      const double run = std::hypot(dx, dz);

      if (run < in.toftDepthMetres + 1.)
      {
        continue; // Too shallow to divide: it is all cottage.
      }

      const double ux = dx / run, uz = dz / run;
      const Polygon strip = toPolygon(lot.boundary);

      // Each part is the slice of the strip between two distances from the
      // street, so the divisions follow the plot however it is shaped.
      const auto slice = [&](const double from, const double to, std::vector<Position> &into) {
        if (to <= from)
        {
          return;
        }

        const double wide = 60.;
        const double nx = -uz * wide, nz = ux * wide;
        Polygon band;
        for (const auto &p : std::vector<std::pair<double, double>>{
                 {front.x + ux * from - nx, front.z + uz * from - nz},
                 {front.x + ux * to - nx, front.z + uz * to - nz},
                 {front.x + ux * to + nx, front.z + uz * to + nz},
                 {front.x + ux * from + nx, front.z + uz * from + nz}})
        {
          band.outer.push_back({geom2::toUnit(p.first), geom2::toUnit(p.second)});
        }

        if (!geom2::counterClockwise(band.outer))
        {
          geom2::reverse(band.outer);
        }

        std::vector<Polygon> part;
        if (!geom2::intersectionOf({geom2::toOperand(band), geom2::toOperand(strip)}, part, nullptr) || part.empty())
        {
          return;
        }

        const auto &biggest = *std::max_element(part.begin(), part.end(), [](const Polygon &a, const Polygon &b) {
          return std::abs(geom2::areaMetres(a)) < std::abs(geom2::areaMetres(b));
        });

        if (std::abs(geom2::areaMetres(biggest)) >= 2.)
        {
          into = toRing(biggest.outer);
        }
      };

      double at = 0;
      slice(at, std::min(run, at + in.toftDepthMetres), lot.toft);
      at += in.toftDepthMetres;
      slice(at, std::min(run, at + in.yardDepthMetres), lot.yard);
      at += in.yardDepthMetres;
      slice(at, std::min(run, at + in.penDepthMetres), lot.pen);
      at += in.penDepthMetres;
      slice(at, run, lot.garden);
    }
  }

  // --- what each plot is for ---------------------------------------------
  //
  // A corner is seen from two streets and is worth more as trade than as
  // another cottage, which is why every market town puts its shops on them.
  // A craftsmen's block wants yards rather than shopfronts.
  const bool tradeBlock = !in.crofts && (in.use == DistrictUse::Merchant || in.use == DistrictUse::Guild);
  const bool craftBlock = !in.crofts && in.use == DistrictUse::Craftsmen;

  for (uint32_t k = 0; k < plan.lots.size(); ++k)
  {
    if (in.crofts)
    {
      break;
    }

    auto &lot = plan.lots[k];

    // A corner is seen from two streets and is worth more as trade than as
    // another cottage, which is why every market town puts its shops on them.
    if (lot.corner)
    {
      lot.role = craftBlock ? CityBlockLotRole::Workshop : CityBlockLotRole::ShopHouse;
    }
    else if (tradeBlock)
    {
      // Even a merchants' block is not a wall of shopfronts: the people who
      // keep the shops live on it. Making every plot a shop also left nothing
      // that could carry the way into the middle, so the block had no
      // corridor at all.
      lot.role = (k % 3 == 2) ? CityBlockLotRole::Dwelling : CityBlockLotRole::ShopHouse;
    }
    else if (craftBlock && (k % 3 == 1))
    {
      lot.role = CityBlockLotRole::Workshop;
    }
  }

  // --- what the neighbourhood needs --------------------------------------
  //
  // Placed after the plots are valued and before anything is built, because
  // these are not a property of a plot: a bakehouse is on this block because
  // the block has twenty two households and no bread, and the plot it lands
  // on is simply the best one still going. Chosen by a stable roll off the
  // block's own id so the same block always owes the same things.
  {
    uint32_t households = 0;
    for (const auto &lot : plan.lots)
    {
      if (lot.role == CityBlockLotRole::Dwelling)
      {
        ++households;
      }
    }

    // The best plot still a dwelling: biggest, preferring a corner, and never
    // the one carrying the passage.
    auto claim = [&](const CityBlockLotRole role) {
      uint32_t best = UINT32_MAX;
      double score = -1;

      for (uint32_t k = 0; k < plan.lots.size(); ++k)
      {
        const auto &lot = plan.lots[k];

        if (lot.role != CityBlockLotRole::Dwelling)
        {
          continue;
        }

        const double value = lot.area * (lot.corner ? 1.4 : 1.);

        if (value > score)
        {
          score = value;
          best = k;
        }
      }

      if (best != UINT32_MAX)
      {
        plan.lots[best].role = role;
        return true;
      }

      return false;
    };

    uint32_t streetEdges = 0;
    for (const bool street : in.streetEdges)
    {
      streetEdges += street ? 1u : 0u;
    }

    const uint64_t roll = (in.id * 0x9E3779B97F4A7C15ull + in.seed) >> 33;

    if (in.needs.dwellingsPerPub && households >= in.needs.dwellingsPerPub)
    {
      claim(CityBlockLotRole::Pub);
    }

    if (in.needs.dwellingsPerBakehouse && households >= in.needs.dwellingsPerBakehouse)
    {
      claim(CityBlockLotRole::Bakehouse);
    }

    if (in.needs.smithyInCraftBlocks && craftBlock)
    {
      claim(CityBlockLotRole::Smithy);
    }

    if (in.needs.innOnThroughStreets && streetEdges >= 2 && (roll % 5) == 0)
    {
      claim(CityBlockLotRole::Inn);
    }
  }

  for (const auto &lot : plan.lots)
  {
    if (lot.role == CityBlockLotRole::Dwelling)
    {
      ++plan.dwellings;
    }
    else if (lot.role != CityBlockLotRole::Passage)
    {
      ++plan.trades;
    }
  }

  // --- the way in, and the middle ----------------------------------------
  //
  // Whatever the frontage rows did not take is the middle of the block, and
  // it is worth keeping as a garden rather than filling: it is where the back
  // yards look onto, and every old town has one. But a middle with no way to
  // it is a middle nobody uses, so a corridor is cut from a street through
  // the frontage - and the plot it is cut beside carries it over its ground
  // floor, which is how a town does this without losing a plot.
  {
    std::vector<geom2::Operand> built = paved;
    for (const auto &p : taken)
    {
      built.push_back(geom2::toOperand(p));
    }

    std::vector<Polygon> middle;
    if (!built.empty() && !geom2::differenceOf(geom2::toOperand(block), built, middle, error))
    {
      return false;
    }

    for (auto &piece : middle)
    {
      if (piece.holes.empty() && std::abs(geom2::areaMetres(piece)) >= in.gardenMinimumMetres2)
      {
        plan.gardens.push_back(toRing(piece.outer));
      }
    }
  }

  if (!plan.gardens.empty() && plan.paving.empty())
  {
    // The corridor goes beside the widest plot on the longest street edge:
    // the plot that can best afford to give up the width, and the frontage
    // most people arrive from.
    // The plot that carries the way is a DWELLING that can afford the width,
    // not simply the biggest plot on the block: picking the biggest overwrote
    // whatever the neighbourhood had just been given, because the pub and the
    // bakehouse are put on the best plots for the same reason.
    uint32_t widest = UINT32_MAX;
    for (uint32_t k = 0; k < plan.lots.size(); ++k)
    {
      if (plan.lots[k].role != CityBlockLotRole::Dwelling)
      {
        continue;
      }

      if (widest == UINT32_MAX || plan.lots[k].area > plan.lots[widest].area)
      {
        widest = k;
      }
    }

    if (widest == UINT32_MAX)
    {
      plan.notes.push_back("no dwelling could carry the way into the block");
    }
    else
    {
      plan.lots[widest].role = CityBlockLotRole::Passage;
    }

    // The corridor itself: from the street edge that plot fronts, straight
    // through to the middle.
    size_t edge = 0, seen = 0;
    if (widest == UINT32_MAX) { out = std::move(plan); return true; }
    for (size_t i = 0; i < in.boundary.size(); ++i)
    {
      if (!in.streetEdges[i])
      {
        continue;
      }

      if (seen == plan.lots[widest].frontageEdge)
      {
        edge = i;
        break;
      }

      ++seen;
    }

    const Position a = in.boundary[edge];
    const Position b = in.boundary[(edge + 1) % in.boundary.size()];
    const double dx = b.x - a.x, dz = b.z - a.z, run = std::hypot(dx, dz);

    if (run > 1e-6)
    {
      Position centre{};
      for (const auto &p : plan.lots[widest].boundary)
      {
        centre.x += p.x / double(plan.lots[widest].boundary.size());
        centre.z += p.z / double(plan.lots[widest].boundary.size());
      }

      const double along = std::clamp(((centre.x - a.x) * dx + (centre.z - a.z) * dz) / (run * run), 0., 1.);
      const double sx = a.x + dx * along, sz = a.z + dz * along;
      const double reach = in.sidewalkMetres + in.lotDepthMetres + 1.5;
      const double nx = -dz / run, nz = dx / run;
      const double half = in.corridorMetres * .5;

      Polygon way;
      for (const auto &p : std::vector<std::pair<double, double>>{
               {sx - dx / run * half, sz - dz / run * half},
               {sx + dx / run * half, sz + dz / run * half},
               {sx + dx / run * half + nx * reach, sz + dz / run * half + nz * reach},
               {sx - dx / run * half + nx * reach, sz - dz / run * half + nz * reach}})
      {
        way.outer.push_back({geom2::toUnit(p.first), geom2::toUnit(p.second)});
      }

      if (!geom2::counterClockwise(way.outer))
      {
        geom2::reverse(way.outer);
      }

      std::vector<Polygon> inside;
      if (!geom2::intersectionOf({geom2::toOperand(way), geom2::toOperand(block)}, inside, error))
      {
        return false;
      }

      for (auto &piece : inside)
      {
        if (std::abs(geom2::areaMetres(piece)) >= .8)
        {
          plan.corridors.push_back(toRing(piece.outer));
        }
      }
    }
  }
  else if (plan.paving.empty())
  {
    plan.notes.push_back("block has no middle: the frontage rows meet");
  }

  // Interior plans already own their lane access and programme. Remap their
  // adjacency indices after appending them to the outer frontage lots.
  for(auto &inner:interiorPlans) {
    const uint32_t offset=uint32_t(plan.lots.size());
    for(auto &lot:inner.lots) {
      if(lot.before!=UINT32_MAX)lot.before+=offset;
      if(lot.after!=UINT32_MAX)lot.after+=offset;
      lot.frontageEdge+=uint32_t(in.boundary.size());
      plan.lots.push_back(std::move(lot));
    }
    plan.dwellings+=inner.dwellings;plan.trades+=inner.trades;
  }

  // A courtyard is frontage only for plots with a real inner edge. Keep the
  // remaining houses street facing; they must not acquire a door toward a
  // court across another building. Offset courts follow the block outline,
  // including angled and concave sides.
  if(in.inwardCourt) for(auto &lot:plan.lots)for(const auto &court:plan.paving) {
    if(lot.courtFacing)break;
    for(size_t i=0;i<lot.boundary.size() && !lot.courtFacing;++i) {
      const auto a=lot.boundary[i],b=lot.boundary[(i+1)%lot.boundary.size()];
      if(std::hypot(b.x-a.x,b.z-a.z)<in.minimumLotWidthMetres*.7)continue;
      const Position mid{(a.x+b.x)*.5,0,(a.z+b.z)*.5};
      for(size_t j=0;j<court.size();++j) {
        const auto c=court[j],d=court[(j+1)%court.size()];
        const double dx=d.x-c.x,dz=d.z-c.z,length=std::hypot(dx,dz);
        if(length<1.)continue;
        const double t=std::clamp(((mid.x-c.x)*dx+(mid.z-c.z)*dz)/(length*length),0.,1.);
        if(std::hypot(mid.x-c.x-dx*t,mid.z-c.z-dz*t)<2.5) {
          lot.courtFacing=true;break;
        }
      }
    }
  }

  // Sequential lots are neighbours only when they really share a party line.
  // A reserved entrance or a rejected sliver can separate consecutive indices.
  const auto touching=[](const CityBlockLot &a,const CityBlockLot &b) {
    for(size_t i=0;i<a.boundary.size();++i) {
      const auto p=a.boundary[i],q=a.boundary[(i+1)%a.boundary.size()];
      const double dx=q.x-p.x,dz=q.z-p.z,len=std::hypot(dx,dz);
      if(len<.5)continue;
      for(size_t j=0;j<b.boundary.size();++j) {
        const auto r=b.boundary[j],s=b.boundary[(j+1)%b.boundary.size()];
        if(std::abs((r.x-p.x)*dz-(r.z-p.z)*dx)>len*.001 ||
           std::abs((s.x-p.x)*dz-(s.z-p.z)*dx)>len*.001)continue;
        const double u=((r.x-p.x)*dx+(r.z-p.z)*dz)/len,v=((s.x-p.x)*dx+(s.z-p.z)*dz)/len;
        if(std::min(len,std::max(u,v))-std::max(0.,std::min(u,v))>.5)return true;
      }
    }
    return false;
  };
  for(auto &lot:plan.lots) {
    if(lot.before!=UINT32_MAX && !touching(lot,plan.lots[lot.before]))lot.before=UINT32_MAX;
    if(lot.after!=UINT32_MAX && !touching(lot,plan.lots[lot.after]))lot.after=UINT32_MAX;
  }
  out = std::move(plan);

  return true;
}

} // namespace procedural::world::generator
