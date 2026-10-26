#pragma once
#include "WorldCityCompletion.hpp"
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace historical_growth_detail {
inline double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

inline double distance(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

inline double pointSegment(Position p, Position a, Position b)
{
  const double x = b.x - a.x, z = b.z - a.z, d = x * x + z * z;
  const double t = d > 0 ? std::clamp(((p.x - a.x) * x + (p.z - a.z) * z) / d, 0., 1.) : 0.;
  return std::hypot(p.x - a.x - t * x, p.z - a.z - t * z);
}

inline bool intersection(Position a, Position b, Position c, Position d, Position &p)
{
  const double x = b.x - a.x, z = b.z - a.z, u = d.x - c.x, v = d.z - c.z, den = x * v - z * u;
  if (std::abs(den) < 1e-9)
  {
    return false;
  }
  const double t = ((c.x - a.x) * v - (c.z - a.z) * u) / den, s = ((c.x - a.x) * z - (c.z - a.z) * x) / den;
  if (t < 0 || t > 1 || s < 0 || s > 1)
  {
    return false;
  }
  p = {a.x + t * x, 0, a.z + t * z};
  return true;
}

inline std::vector<Position> hull(std::vector<Position> p)
{
  std::sort(p.begin(), p.end(), [](auto a, auto b) {
    return std::tie(a.x, a.z) < std::tie(b.x, b.z);
  });
  p.erase(
      std::unique(
          p.begin(),
          p.end(),
          [](auto a, auto b) {
            return distance(a, b) < 1e-7;
          }),
      p.end());
  if (p.size() < 3)
  {
    return {};
  }
  std::vector<Position> h;
  for (int pass = 0; pass < 2; ++pass)
  {
    const auto base = h.size();
    for (auto v : p)
    {
      while (h.size() >= base + 2 && cross(h[h.size() - 2], h.back(), v) <= 1e-8)
      {
        h.pop_back();
      }
      h.push_back(v);
    }
    h.pop_back();
    std::reverse(p.begin(), p.end());
  }
  return h;
}
} // namespace historical_growth_detail

// Project dated identities onto this generation's occupied lots, independently
// of any previous mesh. Expansion records count actual occupied blocks; walls
// are physical only when the history says they closed and their corridor fits.
inline bool projectHistoricalCityGrowthCandidate(
    const std::vector<SettlementHistoricalBuilding> &buildings,
    const std::vector<SettlementHistoricalEnclosure> &enclosures,
    const CityCompletionPlan &city,
    const CityGrowthPlan &base,
    CityGrowthPlan &result,
    double margin,
    std::string *error)
{
  using namespace historical_growth_detail;
  auto fail = [&](const char *why) {
    if (error)
    {
      *error = why;
    }
    return false;
  };
  std::map<uint64_t, const CitySkeletonRegion *> lots;
  for (const auto &r : city.skeleton.regions)
  {
    if (r.kind == CityRegionKind::Lot)
    {
      lots[r.id] = &r;
    }
  }
  std::map<uint64_t, const CitySkeletonRegion *> occupied;
  for (const auto &b : city.historicalLots)
  {
    if (b.lot && lots.count(b.lot))
    {
      occupied[b.building] = lots.at(b.lot);
    }
  }
  std::map<uint64_t, uint32_t> constructionDates;
  for (const auto &building : buildings)
  {
    constructionDates[building.id] = building.builtWeek;
  }
  auto records = buildings;
  std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
    return std::tie(a.builtWeek, a.id) < std::tie(b.builtWeek, b.id);
  });
  CityGrowthPlan out = base;
  out.historical = true;
  out.walls.clear();
  out.traces.clear();
  out.epochs.clear();
  out.omissions.clear();
  std::set<uint64_t> ids, activeBlocks;
  double area = 0;
  uint32_t count = 0;
  for (const auto &b : records)
  {
    if (!b.id || !ids.insert(b.id).second)
    {
      return fail("duplicate historical growth building");
    }
    if (!occupied.count(b.id))
    {
      continue;
    }
    const auto &lot = *occupied.at(b.id);
    area += lot.area;
    ++count;
    activeBlocks.insert(lot.parent);
    if (out.epochs.empty() || out.epochs.back().builtWeek != b.builtWeek)
    {
      out.epochs.push_back({});
    }
    auto &epoch = out.epochs.back();
    epoch.builtWeek = b.builtWeek;
    epoch.buildings = count;
    epoch.occupiedArea = area;
    epoch.blocks.assign(activeBlocks.begin(), activeBlocks.end());
  }
  auto circuits = enclosures;
  std::sort(circuits.begin(), circuits.end(), [](const auto &a, const auto &b) {
    return std::tie(a.closedWeek, a.era, a.id) < std::tie(b.closedWeek, b.era, b.id);
  });
  ids.clear();
  for (const auto &record : circuits)
  {
    if (!record.id || !ids.insert(record.id).second)
    {
      return fail("duplicate historical enclosure");
    }
    if (!record.closed || record.abandoned || record.palisade)
    {
      continue;
    }
    std::vector<Position> points;
    std::set<uint64_t> members;
    // The ledger's present-day membership can include later infill in an old
    // block. Those houses must not move the wall's original construction line.
    for (auto id : record.buildings)
    {
      if (!constructionDates.count(id) || constructionDates.at(id) <= record.closedWeek)
      {
        members.insert(id);
      }
    }
    for (auto id : members)
    {
      if (occupied.count(id))
      {
        for (auto p : occupied.at(id)->boundary)
        {
          for (auto delta : std::vector<Position>{{-margin, 0, -margin}, {margin, 0, -margin}, {margin, 0, margin}, {-margin, 0, margin}})
          {
            points.push_back({p.x + delta.x, 0, p.z + delta.z});
          }
        }
      }
    }
    const size_t unresolved = std::count_if(members.begin(), members.end(), [&](auto id) {
      return !occupied.count(id);
    });
    if (unresolved)
    {
      out.omissions.push_back("enclosure " + std::to_string(record.id) + ": " + std::to_string(unresolved) + " member footprints unresolved");
    }
    // Close the enclosure around obstructing parcels rather than cutting a
    // later house in half. These are infill parcels inside the reconstructed
    // circuit, not newly invented historical buildings. Bound the expansion.
    auto candidate = hull(points);
    const auto polygonArea = [](const auto &ring) {
      double value = 0.;
      for (size_t i = 0; i < ring.size(); ++i)
      {
        value += ring[i].x * ring[(i + 1) % ring.size()].z - ring[(i + 1) % ring.size()].x * ring[i].z;
      }
      return std::abs(value) * .5;
    };
    const double initialArea = polygonArea(candidate);
    std::set<uint64_t> enclosedLots;
    bool overBudget = false;
    for (unsigned attempt = 0; attempt < 32 && candidate.size() >= 3; ++attempt)
    {
      bool expanded = false;
      for (const auto &[id, lot] : occupied)
      {
        if (!enclosedLots.count(lot->id))
        {
          bool collision = false;
          for (size_t i = 0; i < candidate.size(); ++i)
          {
            for (size_t k = 0; k < lot->boundary.size(); ++k)
            {
              const auto a = candidate[i], b = candidate[(i + 1) % candidate.size()], c = lot->boundary[k], d = lot->boundary[(k + 1) % lot->boundary.size()];
              Position hit;
              collision = collision || intersection(a, b, c, d, hit) || std::min({pointSegment(c, a, b), pointSegment(d, a, b), pointSegment(a, c, d), pointSegment(b, c, d)}) < 1.;
            }
          }
          if (collision)
          {
            enclosedLots.insert(lot->id);
            expanded = true;
            for (auto p : lot->boundary)
            {
              for (auto delta : std::vector<Position>{{-margin, 0, -margin}, {margin, 0, -margin}, {margin, 0, margin}, {-margin, 0, margin}})
              {
                points.push_back({p.x + delta.x, 0, p.z + delta.z});
              }
            }
          }
        }
      }
      if (!expanded)
      {
        break;
      }
      candidate = hull(points);
      if (polygonArea(candidate) > initialArea * 2.5)
      {
        overBudget = true;
        break;
      }
    }
    CityGrowthWall wall;
    wall.id = record.id;
    wall.era = record.era;
    wall.palisade = record.palisade;
    wall.lifecycle = record.absorbed ? CityWallLifecycle::Absorbed : CityWallLifecycle::Standing;
    wall.boundary = std::move(candidate);
    auto omit = [&](const char *why) {
      out.omissions.push_back("enclosure " + std::to_string(record.id) + ": " + why);
    };
    if (wall.boundary.size() < 3)
    {
      omit("no occupied historical footprint");
      continue;
    }
    bool clear = !overBudget;
    bool lotConflict = false;
    for (size_t i = 0; i < wall.boundary.size(); ++i)
    {
      const auto a = wall.boundary[i], b = wall.boundary[(i + 1) % wall.boundary.size()];
      wall.perimeter += distance(a, b);
      wall.enclosedArea += (a.x * b.z - b.x * a.z) * .5;
      for (const auto &[id, lot] : occupied)
      {
        for (size_t k = 0; k < lot->boundary.size(); ++k)
        {
          const auto c = lot->boundary[k], d = lot->boundary[(k + 1) % lot->boundary.size()];
          Position p;
          if (intersection(a, b, c, d, p) || std::min({pointSegment(c, a, b), pointSegment(d, a, b), pointSegment(a, c, d), pointSegment(b, c, d)}) < 1.)
          {
            lotConflict = true;
          }
        }
      }
      // Use the retained road paths, not radial projections from the centre.
      for (const auto &road : city.routes)
      {
        for (size_t k = 1; k < road.path.size(); ++k)
        {
          // Clip street width, not just its centreline, even at wall corners.
          const auto r0 = road.path[k - 1], r1 = road.path[k];
          const double roadRun = distance(r0, r1), padding = road.halfWidth + .5;
          if (roadRun > 1e-6)
          {
            const double ux = (r1.x - r0.x) / roadRun, uz = (r1.z - r0.z) / roadRun;
            double lo = 0, hi = 1;
            const auto clip = [&](double origin, double delta, double minimum, double maximum) {
              if (std::abs(delta) < 1e-9)
              {
                return origin >= minimum && origin <= maximum;
              }
              double first = (minimum - origin) / delta, last = (maximum - origin) / delta;
              if (first > last)
              {
                std::swap(first, last);
              }
              lo = std::max(lo, first);
              hi = std::min(hi, last);
              return lo < hi;
            };
            if (clip((a.x - r0.x) * ux + (a.z - r0.z) * uz, (b.x - a.x) * ux + (b.z - a.z) * uz, -padding, roadRun + padding) &&
                clip(-(a.x - r0.x) * uz + (a.z - r0.z) * ux, -(b.x - a.x) * uz + (b.z - a.z) * ux, -padding, padding))
            {
              wall.openings.push_back({i, lo, hi});
            }
          }
          Position hit;
          if (!intersection(a, b, road.path[k - 1], road.path[k], hit))
          {
            continue;
          }
          const double roadLength = distance(road.path[k - 1], road.path[k]);
          const double sine = roadLength > 0 ? std::abs(cross(a, b, road.path[k]) - cross(a, b, road.path[k - 1])) / (distance(a, b) * roadLength) : 0;
          const double width = std::max(5., (2 * road.halfWidth + 1.) / std::max(.1, sine));
          bool duplicate = false;
          for (size_t g = 0; g < wall.gates.size(); ++g)
          {
            if (distance(hit, wall.gates[g]) < 1e-5)
            {
              wall.gateWidths[g] = std::max(width, wall.gateWidths[g]);
              duplicate = true;
              break;
            }
          }
          if (!duplicate)
          {
            wall.gates.push_back(hit);
            wall.gateWidths.push_back(width);
          }
        }
      }
    }
    // Overlapping road openings merge naturally during wall clipping. Only
    // isolated straight openings receive a gatehouse; corner crossings stay open.
    wall.gatehouseFits.assign(wall.gates.size(), true);
    for (size_t g = 0; g < wall.gates.size(); ++g)
    {
      for (auto v : wall.boundary)
      {
        if (distance(v, wall.gates[g]) < wall.gateWidths[g] * .5 + 2)
        {
          wall.gatehouseFits[g] = false;
        }
      }
      for (size_t h = 0; h < wall.gates.size(); ++h)
      {
        if (g != h && distance(wall.gates[g], wall.gates[h]) < (wall.gateWidths[g] + wall.gateWidths[h]) * .5 + 2)
        {
          wall.gatehouseFits[g] = false;
        }
      }
    }
    for (const auto &earlier : out.walls)
    {
      for (size_t i = 0; i < wall.boundary.size(); ++i)
      {
        for (size_t j = 0; j < earlier.boundary.size(); ++j)
        {
          Position p;
          const auto a = wall.boundary[i], b = wall.boundary[(i + 1) % wall.boundary.size()], c = earlier.boundary[j], d = earlier.boundary[(j + 1) % earlier.boundary.size()];
          if (intersection(a, b, c, d, p) || std::min({pointSegment(a, c, d), pointSegment(b, c, d), pointSegment(c, a, b), pointSegment(d, a, b)}) < 3.)
          {
            clear = false;
          }
        }
      }
    }
    if (overBudget)
    {
      omit("parcel closure exceeds bounded enclosure expansion");
      continue;
    }
    if (lotConflict)
    {
      omit("wall corridor intersects occupied lots");
      continue;
    }
    if (!clear)
    {
      omit("wall corridor intersects another historical circuit");
      continue;
    }
    if (wall.gates.empty())
    {
      omit("no road crossing to provide access");
      continue;
    }
    // Palisades do not receive speculative stone towers.
    if (!wall.palisade)
    {
      for (auto v : wall.boundary)
      {
        bool fits = true;
        for (const auto &road : city.routes)
        {
          for (size_t k = 1; k < road.path.size(); ++k)
          {
            if (pointSegment(v, road.path[k - 1], road.path[k]) < road.halfWidth + 3.)
            {
              fits = false;
            }
          }
        }
        if (fits)
        {
          wall.towers.push_back(v);
        }
      }
    }
    out.walls.push_back(std::move(wall));
  }
  result = std::move(out);
  if (error)
  {
    error->clear();
  }
  return true;
}

// Bounded geometric backtracking: move the candidate into its reserved band
// when a road happens to meet a polygon corner. Every trial reruns clearance
// and gate checks; tolerances and parcel obstacles are never relaxed.
inline bool projectHistoricalCityGrowth(
    const std::vector<SettlementHistoricalBuilding> &buildings,
    const std::vector<SettlementHistoricalEnclosure> &enclosures,
    const CityCompletionPlan &city,
    const CityGrowthPlan &base,
    CityGrowthPlan &result,
    std::string *error = nullptr)
{
  CityGrowthPlan best;
  const auto wanted = std::count_if(enclosures.begin(), enclosures.end(), [](const auto &c) {
    return c.closed && !c.abandoned && !c.palisade;
  });
  for (double margin : {4., 4.5, 5., 5.5, 6.})
  {
    CityGrowthPlan trial;
    if (!projectHistoricalCityGrowthCandidate(buildings, enclosures, city, base, trial, margin, error))
    {
      return false;
    }
    if (margin == 4. || trial.walls.size() > best.walls.size() || (trial.walls.size() == best.walls.size() && trial.omissions.size() < best.omissions.size()))
    {
      best = std::move(trial);
    }
    if (best.walls.size() == size_t(wanted))
    {
      break;
    }
  }
  result = std::move(best);
  return true;
}
} // namespace procedural::world::rewrite
