#pragma once
#include "WorldCityCompletion.hpp"
#include "WorldCityHistoricalGrowth.hpp"
#include "WorldSettlementDirector.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
// Run after access/stair reservations. Each completed ordinary structure gets
// at most one developed lot. Institutions have their separate compound stage.
// No new historical event is emitted by this disposable spatial projection.
inline bool
allocateHistoricalCityLots(const std::vector<SettlementHistoricalBuilding> &records, CityCompletionPlan &plan, std::string *error = nullptr, const std::vector<SettlementHistoricalEnclosure> *enclosures = nullptr,
                           const std::function<double(double,double)> &terrain = {}, const Position *growthCentre = nullptr)
{
  const auto fail = [&](const char *why) {
    if (error)
    {
      *error = why;
    }
    return false;
  };
  std::set<uint64_t> ids;
  const std::set<std::string> uses = {"home", "shop", "inn", "workshop", "farm", "market", "church", "keep", "manor"};
  for (const auto &b : records)
  {
    if (!b.id || !ids.insert(b.id).second || !uses.count(b.use) || !b.floors || b.floors > 16 || b.residents > 100000)
    {
      return fail("invalid historical construction request");
    }
  }
  auto result = plan;
  std::map<uint64_t, Position> centres;
  std::map<uint64_t,double> earthwork, lotArea, blockArea;
  std::map<uint64_t, uint64_t> parents;
  std::set<uint64_t> modularPlots, roomyPlots;
  Position centre{};
  double n = 0;
  for (const auto &r : result.skeleton.regions)
  {
    double regionArea=0;
    for(size_t i=0;i<r.boundary.size();++i) {auto a=r.boundary[i],b=r.boundary[(i+1)%r.boundary.size()];regionArea+=a.x*b.z-b.x*a.z;}
    if(r.kind!=CityRegionKind::Lot) blockArea[r.id]=std::abs(regionArea)*.5;
    if (r.kind == CityRegionKind::Lot && !r.boundary.empty())
    {
      Position p{};
      for (auto q : r.boundary)
      {
        p.x += q.x;
        p.z += q.z;
      }
      p.x /= r.boundary.size();
      p.z /= r.boundary.size();
      centres[r.id] = p;
      parents[r.id] = r.parent;
      lotArea[r.id]=std::abs(regionArea)*.5;
      double relief=0,grade=0;
      if(terrain) {
        const double h=terrain(p.x,p.z);
        for(auto v:r.boundary) {
          for(double t:{.5,1.}) {
            const double x=p.x+(v.x-p.x)*t,z=p.z+(v.z-p.z)*t;
            const double delta=std::abs(terrain(x,z)-h);
            relief=std::max(relief,std::isfinite(delta)?delta:1e6);
            grade=std::max(grade,(std::isfinite(delta)?delta:1e6)/std::max(1.,std::hypot(x-p.x,z-p.z)));
          }
        }
      }
      earthwork[r.id]=relief+4*grade;
      // Preliminary space reservation: four 1.2 m dwelling modules plus
      // the 0.9 m inset on each side. The native fitter remains authoritative.
      double area = 0, longest = 0, ux = 1, uz = 0;
      for (size_t i = 0; i < r.boundary.size(); ++i)
      {
        const auto a = r.boundary[i], b = r.boundary[(i + 1) % r.boundary.size()];
        area += a.x * b.z - b.x * a.z;
        const double length = std::hypot(b.x - a.x, b.z - a.z);
        if (length > longest)
        {
          longest = length;
          ux = (b.x - a.x) / length;
          uz = (b.z - a.z) / length;
        }
      }
      double minU = INFINITY, maxU = -INFINITY, minV = INFINITY, maxV = -INFINITY;
      for (auto a : r.boundary)
      {
        const double u = a.x * ux + a.z * uz, v = -a.x * uz + a.z * ux;
        minU = std::min(minU, u);
        maxU = std::max(maxU, u);
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
      }
      // Attached plots need no detached-house side yards. Applying the
      // 6.6 m detached square here discarded almost every 5 m plaza frontage.
      const double moduleSpan = r.attached ? 3.2 : 6.6;
      const double halfSpan = moduleSpan*.5;
      if (std::abs(area) * .5 >= std::max(35.,moduleSpan*moduleSpan) - 1e-6 && std::min(maxU - minU, maxV - minV) >= moduleSpan - 1e-6)
      {
        modularPlots.insert(r.id);
        // Prefer an actual modular square to a thin wedge within the same
        // block. Irregular lots remain available to the native shape fitter;
        // requiring a square would unnecessarily discard valid L-shaped homes.
        const auto contains = [&](Position q) {
          bool inside = false;
          for (size_t i = 0, j = r.boundary.size() - 1; i < r.boundary.size(); j = i++)
          {
            const auto a = r.boundary[i], b = r.boundary[j];
            if (historical_growth_detail::pointSegment(q, a, b) < 1e-7)
            {
              return true;
            }
            if ((a.z > q.z) != (b.z > q.z) && q.x < (b.x - a.x) * (q.z - a.z) / (b.z - a.z) + a.x)
            {
              inside = !inside;
            }
          }
          return inside;
        };
        bool fits = false;
        for (double fu : {.25, .5, .75})
        {
          for (double fv : {.25, .5, .75})
          {
            const double cu = minU + halfSpan + (maxU - minU - moduleSpan) * fu, cv = minV + halfSpan + (maxV - minV - moduleSpan) * fv;
            std::vector<Position> square;
            for (auto [du, dv] : {std::pair{-halfSpan, -halfSpan}, std::pair{halfSpan, -halfSpan}, std::pair{halfSpan, halfSpan}, std::pair{-halfSpan, halfSpan}})
            {
              square.push_back({(cu + du) * ux - (cv + dv) * uz, 0, (cu + du) * uz + (cv + dv) * ux});
            }
            bool clear = true;
            for (auto q : square)
            {
              clear &= contains(q);
            }
            // A concavity may cut an edge even when all four corners fit.
            for (size_t e = 0; e < 4 && clear; ++e)
            {
              for (size_t k = 0; k < r.boundary.size(); ++k)
              {
                Position hit;
                if (historical_growth_detail::intersection(square[e], square[(e + 1) % 4], r.boundary[k], r.boundary[(k + 1) % r.boundary.size()], hit) && historical_growth_detail::distance(hit, square[e]) > 1e-6 &&
                    historical_growth_detail::distance(hit, square[(e + 1) % 4]) > 1e-6)
                {
                  clear = false;
                }
              }
            }
            fits |= clear;
          }
        }
        if (fits)
        {
          roomyPlots.insert(r.id);
        }
        else if (std::abs(area)*.5 < 50)
        {
          // Newly admitted small lots must actually hold the modular square.
          // Keep larger irregular plots available to the native shape fitter.
          modularPlots.erase(r.id);
        }
      }
      centre.x += p.x;
      centre.z += p.z;
      ++n;
    }
  }
  if (n)
  {
    centre.x /= n;
    centre.z /= n;
  }
  if(growthCentre)centre=*growthCentre;
  std::vector<size_t> candidates;
  for (size_t i = 0; i < result.lots.size(); ++i)
  {
    if (result.lots[i].developed && centres.count(result.lots[i].lot) && modularPlots.count(result.lots[i].lot))
    {
      candidates.push_back(i);
    }
    result.lots[i].developed = false;
  }
  // Occupy street frontage before backland. This follows the paper's useful
  // ordering without pretending that radial Voronoi cells are finished plots.
  std::map<uint64_t, bool> fronted;
  for (const auto &region : result.skeleton.regions)
  {
    if (modularPlots.count(region.id))
    {
      double gap = INFINITY;
      for (const auto &route : result.routes)
      {
        for (size_t k = 1; k < route.path.size(); ++k)
        {
          const auto a = route.path[k - 1], b = route.path[k];
          const double dx = b.x - a.x, dz = b.z - a.z, den = dx * dx + dz * dz;
          if (den < 1e-9)
          {
            continue;
          }
          for (size_t j = 0; j < region.boundary.size(); ++j)
          {
            const auto c = region.boundary[j], d = region.boundary[(j + 1) % region.boundary.size()];
            const Position p{(c.x + d.x) * .5, 0, (c.z + d.z) * .5};
            const double t = std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / den, 0., 1.);
            gap = std::min(gap, std::hypot(p.x - a.x - t * dx, p.z - a.z - t * dz) - route.halfWidth);
          }
        }
      }
      fronted[region.id] = gap <= 1.;
    }
  }
  // Fill the nearest block before opening another block. A radial lot-only
  // sort spreads a few houses over many blocks and reserves empty streets.
  std::map<uint64_t, double> blockDistance;
  for (auto i : candidates)
  {
    const auto id = result.lots[i].lot, pid = parents.at(id);
    const auto p = centres.at(id);
    const double d = std::hypot(p.x - centre.x, p.z - centre.z);
    auto found = blockDistance.find(pid);
    if (found == blockDistance.end() || d < found->second)
    {
      blockDistance[pid] = d;
    }
  }
  // Advance a compact frontier: prefer the next block beside an already
  // occupied block over opening another disconnected radial pocket.
  std::map<uint64_t, Position> blockCentres;
  std::map<uint64_t, size_t> blockCounts, blockOrder;
  std::map<uint64_t,double> blockEarthwork, blockLots;
  for (auto i : candidates)
  {
    const auto id = result.lots[i].lot, pid = parents.at(id);
    const auto p = centres.at(id);
    blockCentres[pid].x += p.x;
    blockCentres[pid].z += p.z;
    ++blockCounts[pid];
    blockEarthwork[pid]+=earthwork[id]; blockLots[pid]+=lotArea[id];
  }
  for (auto &[id, p] : blockCentres)
  {
    p.x /= blockCounts[id];
    p.z /= blockCounts[id];
  }
  std::map<uint64_t, double> frontierDistance;
  for (const auto &[id, p] : blockCentres)
  {
    frontierDistance[id] = std::hypot(p.x - centre.x, p.z - centre.z);
  }
  while (blockOrder.size() < blockCentres.size())
  {
    uint64_t selected = 0;
    double best = INFINITY;
    double bestBand = INFINITY;
    for (const auto &[id, p] : blockCentres)
    {
      if (!blockOrder.count(id))
      {
        // Prefer contiguous blocks that can house demand efficiently, with
        // less levelling, before opening a large sparse or steep block.
        const double area=std::max(blockLots[id],blockArea[id]);
        // Fill a walkable ring around the fixed civic centre before opening
        // a cheaper distant district. Terrain and parcel quality break ties
        // within a ring; they cannot seed independent satellite clusters.
        const double band=std::floor(blockDistance.at(id)/16.);
        const double cost = frontierDistance.at(id) + blockDistance.at(id) +
            12*blockEarthwork[id]/blockCounts.at(id) + .35*area/blockCounts.at(id);
        if (band<bestBand || (band==bestBand && (cost < best || (cost == best && id < selected))))
        {
          bestBand=band;
          best = cost;
          selected = id;
        }
      }
    }
    const bool first = blockOrder.empty();
    blockOrder[selected] = blockOrder.size();
    const auto q = blockCentres.at(selected);
    for (const auto &[id, p] : blockCentres)
    {
      if (!blockOrder.count(id))
      {
        const double distance = std::hypot(p.x - q.x, p.z - q.z);
        frontierDistance[id] = first ? distance : std::min(frontierDistance.at(id), distance);
      }
    }
  }
  std::map<uint64_t, const CitySkeletonRegion *> lotRegions;
  for (const auto &region : result.skeleton.regions)
  {
    if (region.kind == CityRegionKind::Lot)
    {
      lotRegions[region.id] = &region;
    }
  }
  std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
    const auto pa = parents.at(result.lots[a].lot), pb = parents.at(result.lots[b].lot);
    // Complete the planned square's street wall before opening scattered
    // peripheral frontages. It consumes real historical buildings, not extra
    // anonymous houses, and preserves the separate farmstead allocation below.
    const bool plazaA = lotRegions.at(result.lots[a].lot)->publicFrontage.size()==2;
    const bool plazaB = lotRegions.at(result.lots[b].lot)->publicFrontage.size()==2;
    if(plazaA!=plazaB)return plazaA;
    if(growthCentre) {
      const auto p=centres.at(result.lots[a].lot),q=centres.at(result.lots[b].lot);
      const double ar=std::floor(std::hypot(p.x-centre.x,p.z-centre.z)/16.);
      const double br=std::floor(std::hypot(q.x-centre.x,q.z-centre.z)/16.);
      if(ar!=br)return ar<br;
    }
    if (pa != pb)
    {
      return blockOrder.at(pa) < blockOrder.at(pb);
    }
    const bool fa = fronted[result.lots[a].lot], fb = fronted[result.lots[b].lot];
    if (fa != fb)
    {
      return fa;
    }
    const bool ra = roomyPlots.count(result.lots[a].lot), rb = roomyPlots.count(result.lots[b].lot);
    if (ra != rb)
    {
      return ra;
    }
    const double ea=earthwork[result.lots[a].lot], eb=earthwork[result.lots[b].lot];
    if(std::floor(ea*4)!=std::floor(eb*4)) return ea<eb;
    const auto p = centres.at(result.lots[a].lot), q = centres.at(result.lots[b].lot);
    const double da = std::hypot(p.x - centre.x, p.z - centre.z), db = std::hypot(q.x - centre.x, q.z - centre.z);
    return da == db ? result.lots[a].lot < result.lots[b].lot : da < db;
  });
  auto buildings = records;
  std::sort(buildings.begin(), buildings.end(), [](const auto &a, const auto &b) {
    return std::tie(a.builtWeek, a.id) < std::tie(b.builtWeek, b.id);
  });
  std::vector<SettlementHistoricalEnclosure> dated = enclosures ? *enclosures : std::vector<SettlementHistoricalEnclosure>{};
  std::sort(dated.begin(), dated.end(), [](const auto &a, const auto &b) {
    return std::tie(a.closedWeek, a.id) < std::tie(b.closedWeek, b.id);
  });
  std::vector<std::vector<Position>> reservedWalls;
  size_t nextCircuit = 0;
  result.historicalLots.clear();
  size_t next = 0;
  const size_t ordinaryDemand=std::count_if(buildings.begin(),buildings.end(),[](const auto &b){return b.use!="keep" && b.use!="manor" && b.use!="church";});
  const size_t occupiedLimit=std::min(candidates.size(),ordinaryDemand);
  std::set<size_t> assigned;
  for (const auto &b : buildings)
  {
    // Close dated corridors before placing later buildings. This is the
    // boundary between an inner historical district and subsequent infill.
    while (nextCircuit < dated.size() && dated[nextCircuit].closedWeek < b.builtWeek)
    {
      const auto &c = dated[nextCircuit++];
      if (!c.closed || c.abandoned || c.palisade)
      {
        continue;
      }
      const std::set<uint64_t> members(c.buildings.begin(), c.buildings.end());
      std::vector<Position> points;
      for (const auto &binding : result.historicalLots)
      {
        if (binding.lot && members.count(binding.building))
        {
          for (auto p : lotRegions.at(binding.lot)->boundary)
          {
            for (auto delta : std::vector<Position>{{-4, 0, -4}, {4, 0, -4}, {4, 0, 4}, {-4, 0, 4}})
            {
              points.push_back({p.x + delta.x, 0, p.z + delta.z});
            }
          }
        }
      }
      auto ring = historical_growth_detail::hull(std::move(points));
      if (ring.size() >= 3)
      {
        reservedWalls.push_back(std::move(ring));
      }
    }
    const auto fitsCorridors = [&](uint64_t lotId) {
      using namespace historical_growth_detail;
      const auto &ring = lotRegions.at(lotId)->boundary;
      for (const auto &wall : reservedWalls)
      {
        for (size_t i = 0; i < wall.size(); ++i)
        {
          for (size_t j = 0; j < ring.size(); ++j)
          {
            const auto a = wall[i], b = wall[(i + 1) % wall.size()], c = ring[j], d = ring[(j + 1) % ring.size()];
            Position hit;
            if (intersection(a, b, c, d, hit) || std::min({pointSegment(a, c, d), pointSegment(b, c, d), pointSegment(c, a, b), pointSegment(d, a, b)}) < 3.)
            {
              return false;
            }
          }
        }
      }
      return true;
    };
    if (b.use == "keep" || b.use == "manor" || b.use == "church")continue;
    const double footprint = std::max(40., std::max(2u, (b.residents + 1) / 2) * 15. / b.floors);
    size_t selected=candidates.size();
    const auto farmsteadLot=[&](size_t i){const auto r=lotRegions.find(result.lots[candidates[i]].lot);return r!=lotRegions.end() && r->second->lotProgramme=="farmstead";};
    // A farm goes to a farmstead lot - the farmhouse site of a holding in
    // the hinterland - while one is free; only then, as before, to an urban
    // lot on the edge of the occupied area. A farmstead lot is never spent
    // on anything but a farm.
    if(b.use=="farm")for(size_t i=0;i<candidates.size();++i)
      if(!assigned.count(i) && farmsteadLot(i)){selected=i;break;}
    if(b.use=="farm" && selected==candidates.size())for(size_t i=occupiedLimit;i-->0;)
      if(!assigned.count(i) && !farmsteadLot(i) && fitsCorridors(result.lots[candidates[i]].lot)){selected=i;break;}
    if(selected==candidates.size()) {
      while(next<candidates.size() && (assigned.count(next) || farmsteadLot(next) || !fitsCorridors(result.lots[candidates[next]].lot)))++next;
      selected=next;
    }
    if(selected==candidates.size()){result.historicalLots.push_back({b.id,0,footprint});continue;}
    assigned.insert(selected);
    auto &lot=result.lots[candidates[selected]];
    lot.developed = true;
    // Historical demand is a minimum programme, not a cap on the street wall.
    // Urban houses occupy most of a normal parcel, leaving a rear yard. Large
    // gardens and farms must not turn into another unbounded estate wing.
    lot.preferredFootprintMetres2 = b.use == "farm" ? footprint :
        std::max(footprint, std::min(120., lotArea.at(lot.lot) * .85));
    lot.rowHouse = b.use != "farm";
    lot.minimumFloors = lot.maximumFloors = b.floors;
    lot.merchantHouse = b.use == "shop" || b.use == "market" || b.use == "workshop";
    lot.residence = b.use == "inn" ? CityResidenceKind::Inn : CityResidenceKind::House;
    lot.desiredRooms = std::max(2u, (b.residents + 1) / 2);
    // A home on a large lot of the wealthy quarter is the mansion or manor
    // completion planned there (WorldCityCompletion), not a row house: the
    // history names who lives in it, the district says how they live. Floors
    // stay the history's, which the release check compares.
    if (b.use == "home")
      if (const auto r = lotRegions.find(lot.lot); r != lotRegions.end() &&
          (r->second->districtUse == DistrictUse::WealthyResidential || r->second->districtUse == DistrictUse::Manor) &&
          lotArea.at(lot.lot) >= 180.)
      {
        const bool manor = lotArea.at(lot.lot) >= 420.;
        lot.residence = manor ? CityResidenceKind::Manor : CityResidenceKind::Mansion;
        lot.rowHouse = false;
        lot.preferredFootprintMetres2 = manor ? 200. : 140.;
        lot.desiredRooms = manor ? 28u : 22u;
        lot.preferredRoomMetres2 = 20.;
      }
    // The trade room follows the workplace the history put here; a farm is
    // a farmstead whoever works it, and a workshop with no named trade is a
    // carpenter's.
    lot.architecturalAgeYears=b.architecturalAgeYears;
    lot.trade = !b.trade.empty() ? b.trade : b.use == "farm" ? "farmstead" : b.use == "workshop" ? "carpentry" : "";
    if (!lot.trade.empty() && lot.trade != "hall")
    {
      lot.desiredRooms = std::max(lot.desiredRooms, 3u);
    }
    result.historicalLots.push_back({b.id, lot.lot, footprint});
  }
  plan = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
