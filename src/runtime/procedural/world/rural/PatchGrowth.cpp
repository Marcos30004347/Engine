#include "procedural/world/rural/PatchGrowth.hpp"

#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>

namespace procedural::world::rural {

using namespace ::procedural::world::rewrite;

bool GridGrowthStrategy::grow(const GrowthContext &c, std::vector<Growth> &growth, std::string *error)
{
  const WorldSuitability &w = c.suitability;
  const RuralPatchSettings &s = c.settings;
  const auto &src = c.sources;
  const auto &regions = c.regions;
  const auto &clearance = c.clearance;
  const auto &reservedCell = c.reservedCell;
  const double h = c.halfSpacing;
  auto &owner = c.owner;
  auto &r = c.product;

  const size_t n = w.cells().size();

  const auto fail = [&](const char *m) {
    if (error != nullptr)
    {
      *error = m;
    }

    return false;
  };

  (void)w;
  (void)s;
  (void)src;
  (void)regions;
  (void)clearance;
  (void)reservedCell;
  (void)h;
  (void)owner;
  (void)r;
  (void)n;

  int radius = int(std::ceil(s.minimumWidth / (2 * h))) + 1;
  if (radius > 32)
  {
    return fail("rural support kernel budget exceeded");
  }
  auto kernel = [&](uint32_t i) {
    std::vector<uint32_t> k;
    int x = i % w.columns(), z = i / w.columns();
    for (int dz = -radius; dz <= radius; ++dz)
    {
      for (int dx = -radius; dx <= radius; ++dx)
      {
        if (dx * dx + dz * dz > radius * radius)
        {
          continue;
        }
        int a = x + dx, b = z + dz;
        if (a < 0 || b < 0 || a >= int(w.columns()) || b >= int(w.rows()))
        {
          return std::vector<uint32_t>{};
        }
        k.push_back(b * w.columns() + a);
      }
    }
    return k;
  };
  std::vector<uint32_t> candidates;
  for (uint32_t i = 0; i < n; ++i)
  {
    if (r.eligible[i] && r.catchments[i] && clearance[i] >= uint32_t(radius))
    {
      candidates.push_back(i);
    }
  }
  auto score = [&](uint32_t i) {
    return w.cells()[i].score * 10 + std::min(clearance[i] * h, 100.) * .05 - r.travelCost[i] * .006 + double(featureSeed(s.seed, 12, i, 1) % 1000) * .00001;
  };
  std::sort(candidates.begin(), candidates.end(), [&](auto a, auto b) {
    double x = score(a), y = score(b);
    return x == y ? a < b : x > y;
  });

  auto valid = [&](const std::vector<uint32_t> &k, uint64_t source, uint32_t patch) {
    if (k.empty())
    {
      return false;
    }
    for (auto j : k)
    {
      if (!r.eligible[j] || r.catchments[j] != source || (owner[j] && owner[j] != patch))
      {
        return false;
      }
    }
    return true;
  };

  // Seed at most the explicitly budgeted number of broad cores; competing sources
  // use the same deterministic catchment/ownership arrays.
  for (auto i : candidates)
  {
    if (growth.size() >= s.maximumPatches)
    {
      break;
    }
    auto b = std::find_if(r.budgets.begin(), r.budgets.end(), [&](auto b) {
      return b.source == r.catchments[i];
    });
    double reserved = 0;
    for (auto &g : growth)
    {
      if (g.source == b->source)
      {
        reserved += g.budget;
      }
    }
    double budget = std::min(s.maximumArea, b->requested - reserved);
    auto k = kernel(i);
    if (budget < std::max(s.minimumArea, k.size() * h * h) || !valid(k, b->source, 0))
    {
      continue;
    }
    bool spaced = true;
    for (auto &g : growth)
    {
      auto a = w.cells()[g.seed].center, c = w.cells()[i].center;
      if (std::hypot(a.x - c.x, a.z - c.z) < std::sqrt(s.maximumArea) * 1.3 + s.patchSpacing)
      {
        spaced = false;
      }
    }
    if (!spaced)
    {
      continue;
    }
    growth.push_back({i, b->source, budget, {k.begin(), k.end()}, {}});
    for (auto j : k)
    {
      owner[j] = growth.size();
    }
  }
  using Grow = std::tuple<double, uint32_t, uint32_t>;
  std::priority_queue<Grow, std::vector<Grow>, std::greater<Grow>> frontier;
  auto enqueue = [&](uint32_t p, uint32_t i) {
    auto &g = growth[p - 1];
    if (!g.visited.insert(i).second)
    {
      return;
    }
    auto a = w.cells()[g.seed].center, b = w.cells()[i].center;
    auto support = kernel(i);
    std::set<uint32_t> added;
    for (auto j : support)
    {
      if (!g.cells.count(j))
      {
        added.insert(j);
      }
    }
    double perimeterDelta = 0;
    for (auto j : added)
    {
      perimeterDelta += 4;
      for (auto neighbor : w.neighbors(j))
      {
        perimeterDelta -= g.cells.count(neighbor) ? 2 : added.count(neighbor) ? 1 : 0;
      }
    }
    frontier.emplace(std::hypot(a.x - b.x, a.z - b.z) / h - score(i) + .15 * perimeterDelta, p, i);
  };
  for (uint32_t p = 1; p <= growth.size(); ++p)
  {
    enqueue(p, growth[p - 1].seed);
  }
  while (!frontier.empty())
  {
    auto [priority, p, i] = frontier.top();
    frontier.pop();
    auto &g = growth[p - 1];
    auto k = kernel(i);
    if (!valid(k, g.source, p))
    {
      continue;
    }
    size_t added = 0;
    for (auto j : k)
    {
      added += owner[j] != p;
    }
    if ((g.cells.size() + added) * h * h > g.budget)
    {
      continue;
    }
    bool gap = true;
    int gapCells = std::ceil(s.patchSpacing / h);
    for (auto j : k)
    {
      int x = j % w.columns(), z = j / w.columns();
      for (int dz = -gapCells; dz <= gapCells && gap; ++dz)
      {
        for (int dx = -gapCells; dx <= gapCells; ++dx)
        {
          int a = x + dx, b = z + dz;
          if (a >= 0 && b >= 0 && a < int(w.columns()) && b < int(w.rows()))
          {
            auto o = owner[b * w.columns() + a];
            if (o && o != p)
            {
              gap = false;
              break;
            }
          }
        }
      }
    }
    if (!gap)
    {
      continue;
    }
    for (auto j : k)
    {
      owner[j] = p;
      g.cells.insert(j);
    }
    for (auto j : w.neighbors(i))
    {
      enqueue(p, j);
    }
  }

  return true;
}

bool RegionGrowthStrategy::grow(const GrowthContext &c, std::vector<Growth> &growth, std::string *error)
{
  const WorldSuitability &w = c.suitability;
  const RuralPatchSettings &s = c.settings;
  const auto &src = c.sources;
  const auto &regions = c.regions;
  const auto &clearance = c.clearance;
  const auto &reservedCell = c.reservedCell;
  const double h = c.halfSpacing;
  auto &owner = c.owner;
  auto &r = c.product;

  const size_t n = w.cells().size();

  const auto fail = [&](const char *m) {
    if (error != nullptr)
    {
      *error = m;
    }

    return false;
  };

  (void)w;
  (void)s;
  (void)src;
  (void)regions;
  (void)clearance;
  (void)reservedCell;
  (void)h;
  (void)owner;
  (void)r;
  (void)n;

  int radius = int(std::ceil(s.minimumWidth / (2 * h))) + 1;
  if (radius > 32)
  {
    return fail("rural support kernel budget exceeded");
  }
  auto kernel = [&](uint32_t i) {
    std::vector<uint32_t> k;
    int x = i % w.columns(), z = i / w.columns();
    for (int dz = -radius; dz <= radius; ++dz)
    {
      for (int dx = -radius; dx <= radius; ++dx)
      {
        if (dx * dx + dz * dz > radius * radius)
        {
          continue;
        }
        int a = x + dx, b = z + dz;
        if (a < 0 || b < 0 || a >= int(w.columns()) || b >= int(w.rows()))
        {
          return std::vector<uint32_t>{};
        }
        k.push_back(b * w.columns() + a);
      }
    }
    return k;
  };
  std::vector<uint32_t> candidates;
  for (uint32_t i = 0; i < n; ++i)
  {
    if (r.eligible[i] && r.catchments[i] && clearance[i] >= uint32_t(radius))
    {
      candidates.push_back(i);
    }
  }
  auto score = [&](uint32_t i) {
    return w.cells()[i].score * 10 + std::min(clearance[i] * h, 100.) * .05 - r.travelCost[i] * .006 + double(featureSeed(s.seed, 12, i, 1) % 1000) * .00001;
  };
  std::sort(candidates.begin(), candidates.end(), [&](auto a, auto b) {
    double x = score(a), y = score(b);
    return x == y ? a < b : x > y;
  });

  auto valid = [&](const std::vector<uint32_t> &k, uint64_t source, uint32_t patch) {
    if (k.empty())
    {
      return false;
    }
    for (auto j : k)
    {
      if (!r.eligible[j] || r.catchments[j] != source || (owner[j] && owner[j] != patch))
      {
        return false;
      }
    }
    return true;
  };

  // Whole source cells form adjoining countryside, with no artificial spacing
  // between estates. The grid only audits terrain and provides access paths.
  for (const auto &region : regions)
  {
    if (region.cover != RuralLandCover::Open)
    {
      continue;
    }
    if (region.reserved)
    {
      r.rejections.push_back("original region " + std::to_string(region.id) + " omitted: reserved as the suburb belt");
      continue;
    }
    // The region holding a rural exit carries the road out of the town; the
    // approach lane would cross its own field, so it is never farmed.
    {
      bool carriesExit = false;
      for (const auto &source : src)
      {
        bool insideRegion = true;
        for (size_t k = 0; k < region.boundary.size() && insideRegion; ++k)
        {
          insideRegion = cross(region.boundary[k], region.boundary[(k + 1) % region.boundary.size()], source.exit) >= -1e-8;
        }
        carriesExit |= insideRegion;
      }
      if (carriesExit)
      {
        r.rejections.push_back("original region " + std::to_string(region.id) + " omitted: it carries a rural exit road");
        continue;
      }
    }
    double a = area(region.boundary);
    std::set<uint32_t> support;
    uint32_t anchor = UINT32_MAX;
    double best = INFINITY;
    bool suitable = a >= s.minimumArea && a <= s.maximumArea;
    uint32_t blocked = 0, reservedBlocked = 0;
    Position center{};
    for (auto p : region.boundary)
    {
      center.x += p.x / region.boundary.size();
      center.z += p.z / region.boundary.size();
    }
    // An analysis cell belongs to the region containing its centre, the same
    // ownership the grid mode uses. Cells straddling a shared boundary are
    // judged by their own region; the neighbour's protected cover or urban
    // reservation cannot veto this region through the overlap of a border
    // cell, and the crop polygon is still exactly this region's boundary.
    for (uint32_t i = 0; suitable && i < n; ++i)
    {
      auto p = w.cells()[i].center;
      bool owned = true;
      for (size_t k = 0; k < region.boundary.size() && owned; ++k)
      {
        owned = cross(region.boundary[k], region.boundary[(k + 1) % region.boundary.size()], p) >= -1e-8;
      }
      if (!owned)
      {
        continue;
      }
      if (!r.eligible[i])
      {
        ++blocked;
        reservedBlocked += reservedCell[i];
        continue;
      }
      support.insert(i);
      auto cell = cellPoly(w, i);
      bool inside = true;
      for (auto q : cell)
      {
        for (size_t k = 0; k < region.boundary.size(); ++k)
        {
          if (cross(region.boundary[k], region.boundary[(k + 1) % region.boundary.size()], q) < -1e-8)
          {
            inside = false;
          }
        }
      }
      double distance = std::hypot(p.x - center.x, p.z - center.z);
      if (inside && r.catchments[i] && r.travelCost[i] <= s.maximumAccessCost && distance < best)
      {
        best = distance;
        anchor = i;
      }
    }
    if (!suitable)
    {
      r.rejections.push_back("original region " + std::to_string(region.id) + " lies outside the patch area limits");
      continue;
    }
    // A region is accepted when at most a tenth of its analysis cells are
    // unsuitable: the wall-side regions are otherwise lost to a few steep or
    // wet cells and the farms retreat from the town. The tolerance is recorded.
    // A reserved cell (urban land, a compound) can never be farmed over: the
    // reservation contour would cross the field. Terrain-unsuitable cells are
    // tolerated up to a tenth of the region.
    if (blocked && (reservedBlocked || blocked * 10 > support.size() + blocked))
    {
      if (s.partialRegions)
      {
        // The region is not farmed whole: its suitable part is. The largest
        // four-connected group of eligible cells, outlined on the analysis
        // grid, is the patch (fields carve it as any other), so a few steep
        // or wet cells, or a reservation in one corner, no longer empty the
        // whole region around the town.
        std::vector<uint32_t> component;
        {
          std::set<uint32_t> left = support;
          while (!left.empty())
          {
            std::vector<uint32_t> group{*left.begin()};
            left.erase(left.begin());
            for (size_t q = 0; q < group.size(); ++q)
            {
              const uint32_t i = group[q];
              const uint32_t x = i % w.columns();
              const uint32_t around[] = {x > 0 ? i - 1 : UINT32_MAX, x + 1 < w.columns() ? i + 1 : UINT32_MAX, i >= w.columns() ? i - w.columns() : UINT32_MAX, i + w.columns() < n ? i + w.columns() : UINT32_MAX};
              for (auto j : around)
              {
                if (j != UINT32_MAX && left.erase(j))
                {
                  group.push_back(j);
                }
              }
            }
            if (group.size() > component.size())
            {
              component = std::move(group);
            }
          }
        }
        const double cellArea = w.settings().spacingMetres * w.settings().spacingMetres;
        Poly partial;
        uint32_t partAnchor = UINT32_MAX;
        double partBest = INFINITY;
        for (auto i : component)
        {
          auto p = w.cells()[i].center;
          double distance = std::hypot(p.x - center.x, p.z - center.z);
          if (r.catchments[i] && r.travelCost[i] <= s.maximumAccessCost && distance < partBest)
          {
            partBest = distance;
            partAnchor = i;
          }
        }
        double partArea = component.size() * cellArea;
        // The convex hull of the group's cells is the patch when it adds little
        // area and stays inside the region (few vertices, so few contours);
        // otherwise the grid outline is.
        if (partArea >= s.minimumArea && partAnchor != UINT32_MAX)
        {
          Poly corners;
          for (auto i : component)
          {
            for (auto q : cellPoly(w, i))
            {
              corners.push_back(q);
            }
          }
          std::sort(corners.begin(), corners.end(), [](auto a, auto b) {
            return a.x == b.x ? a.z < b.z : a.x < b.x;
          });
          Poly hull;
          for (int pass = 0; pass < 2; ++pass)
          {
            size_t base = hull.size();
            for (auto q : corners)
            {
              while (hull.size() >= base + 2 && cross(hull[hull.size() - 2], hull.back(), q) <= 0)
              {
                hull.pop_back();
              }
              hull.push_back(q);
            }
            hull.pop_back();
            std::reverse(corners.begin(), corners.end());
          }
          bool insideRegion = hull.size() >= 3;
          for (auto q : hull)
          {
            for (size_t k = 0; k < region.boundary.size() && insideRegion; ++k)
            {
              insideRegion = cross(region.boundary[k], region.boundary[(k + 1) % region.boundary.size()], q) >= -1e-6;
            }
          }
          const double hullArea = insideRegion ? area(hull) : 0;
          if (insideRegion && hullArea <= partArea * 1.3 && hullArea <= s.maximumArea)
          {
            partial = hull;
            partArea = hullArea;
          }
          else if (!outline(w, component, partial))
          {
            partial.clear();
          }
        }
        if (partArea >= s.minimumArea && partArea <= s.maximumArea && partAnchor != UINT32_MAX && partial.size() >= 3)
        {
          auto b = std::find_if(r.budgets.begin(), r.budgets.end(), [&](auto b) {
            return b.source == r.catchments[partAnchor];
          });
          double reservedBudget = 0;
          for (auto &g : growth)
          {
            if (g.source == b->source)
            {
              reservedBudget += g.budget;
            }
          }
          if (growth.size() < s.maximumPatches && partArea <= b->requested - reservedBudget)
          {
            r.rejections.push_back("original region " + std::to_string(region.id) + " farmed on its suitable part: " + std::to_string(component.size()) + " of " + std::to_string(support.size() + blocked) + " cells");
            growth.push_back({partAnchor, b->source, partArea, std::set<uint32_t>(component.begin(), component.end()), {}, partial, region.id});
            continue;
          }
        }
      }
      r.rejections.push_back("original region " + std::to_string(region.id) + " contains " + std::to_string(blocked) + " unsuitable or reserved cells");
      continue;
    }
    if (blocked)
    {
      r.rejections.push_back("original region " + std::to_string(region.id) + " accepted only with " + std::to_string(blocked) + " unsuitable cells among " + std::to_string(support.size() + blocked));
    }
    if (anchor == UINT32_MAX)
    {
      r.rejections.push_back("original region " + std::to_string(region.id) + " has no interior cell reachable within the access cost");
      continue;
    }
    auto b = std::find_if(r.budgets.begin(), r.budgets.end(), [&](auto b) {
      return b.source == r.catchments[anchor];
    });
    double reserved = 0;
    for (auto &g : growth)
    {
      if (g.source == b->source)
      {
        reserved += g.budget;
      }
    }
    if (growth.size() >= s.maximumPatches || a > b->requested - reserved)
    {
      r.rejections.push_back(
          "original region " + std::to_string(region.id) + " exceeds allocation budget (" + std::to_string(int(a)) + " m2 against " + std::to_string(int(b->requested - reserved)) + " left of " +
          std::to_string(int(b->requested)) + ", " + std::to_string(growth.size()) + " patches)");
      continue;
    }
    growth.push_back({anchor, b->source, a, support, {}, region.boundary, region.id});
  }

  return true;
}

bool growPatches(const GrowthContext &c, std::vector<Growth> &out, std::string *error)
{
  // Which answer to "what is a farm" this hinterland calls for. One line,
  // where there used to be a branch with four hundred lines inside it.
  const GrowthStrategyPtr strategy = c.regions.empty() ? GrowthStrategyPtr(std::make_shared<GridGrowthStrategy>()) : GrowthStrategyPtr(std::make_shared<RegionGrowthStrategy>());

  return strategy->grow(c, out, error);
}

} // namespace procedural::world::rural
