#include "WorldRiverGraph.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
Point2 point(Position p)
{
  return {p.x, p.z};
}

double distance(Point2 p, Point2 a, Point2 b)
{
  double dx = b.x - a.x, dz = b.y - a.y, l2 = dx * dx + dz * dz;
  double t = l2 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dz) / l2, 0.0, 1.0) : 0;
  return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dz);
}

bool intersects(Point2 a, Point2 b, Point2 c, Point2 d)
{
  double abC = orient2dExact(a, b, c), abD = orient2dExact(a, b, d), cdA = orient2dExact(c, d, a), cdB = orient2dExact(c, d, b);
  if (((abC > 0 && abD < 0) || (abC < 0 && abD > 0)) && ((cdA > 0 && cdB < 0) || (cdA < 0 && cdB > 0)))
  {
    return true;
  }
  auto on = [](Point2 p, Point2 a, Point2 b) {
    return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
  };
  return (abC == 0 && on(c, a, b)) || (abD == 0 && on(d, a, b)) || (cdA == 0 && on(a, c, d)) || (cdB == 0 && on(b, c, d));
}

double segmentDistance(Point2 a, Point2 b, Point2 c, Point2 d)
{
  if (intersects(a, b, c, d))
  {
    return 0;
  }
  return std::min({distance(a, c, d), distance(b, c, d), distance(c, a, b), distance(d, a, b)});
}

struct CoastSegment
{
  uint64_t id;
  Point2 a, b;
};
} // namespace

bool growRiverGraph(const LandProduct &land, const ReliefControls &relief, const std::vector<RiverMouth> &mouths, const RiverGrowthSettings &s, RiverGrowthProduct &output, std::string *error)
{
  auto fail = [&](const char *message) {
    if (error)
    {
      *error = message;
    }
    return false;
  };
  if (!land.spec.validate(error))
  {
    return false;
  }
  for (double value :
       {s.edgeLengthMetres,
        s.coastClearanceMetres,
        s.separationMetres,
        s.maximumSlope,
        s.elevationWindowMetres,
        s.continuationProbability,
        s.symmetricProbability,
        s.mouthClearanceScale,
        s.maximumCoastalSlope,
        s.maximumElevationPerTerrainLambda})
  {
    if (!std::isfinite(value))
    {
      return fail("Nonfinite river growth control");
    }
  }
  if (mouths.empty() || mouths.size() > s.maximumNodes || s.maximumNodes > 4096 || s.attemptsPerChild == 0 || s.attemptsPerChild > 64 || s.edgeLengthMetres <= 0 || s.coastClearanceMetres < 0 || s.separationMetres <= 0 ||
      s.mouthClearanceScale <= 0 || s.mouthClearanceScale > 1 || s.maximumElevationPerTerrainLambda < 0 || s.maximumCoastalSlope < 0 || s.maximumSlope <= 0 || s.maximumSlope > .25 || s.elevationWindowMetres < 0 ||
      s.continuationProbability < 0 || s.symmetricProbability < 0 || s.continuationProbability + s.symmetricProbability > 1)
  {
    return fail("Invalid river growth budget or probabilities");
  }
  static const bool initialized = []() {
    initialisePredicates();
    return true;
  }();
  (void)initialized;
  std::vector<CoastSegment> coast;
  std::map<uint64_t, CoastSegment> byId;
  for (auto &loop : land.coasts)
  {
    for (auto use : loop.edges)
    {
      auto e = land.boundaries.find(use.edgeId);
      if (!e || e->vertices.size() != 2)
      {
        return fail("River growth requires frozen unsplit coast segments");
      }
      Point2 a = point(e->vertices.front().position), b = point(e->vertices.back().position);
      if (use.reversed)
      {
        std::swap(a, b);
      }
      CoastSegment segment{use.edgeId, a, b};
      coast.push_back(segment);
      if (!byId.emplace(use.edgeId, segment).second)
      {
        return fail("Repeated coastline segment");
      }
    }
  }
  if (coast.empty())
  {
    return fail("Missing physical coastline");
  }
  auto inside = [&](Point2 p) {
    bool hit = false;
    for (auto e : coast)
    {
      if ((e.a.y > p.y) != (e.b.y > p.y) && p.x < (e.b.x - e.a.x) * (p.y - e.a.y) / (e.b.y - e.a.y) + e.a.x)
      {
        hit = !hit;
      }
    }
    return hit;
  };
  auto sortedMouths = mouths;
  std::sort(sortedMouths.begin(), sortedMouths.end(), [](auto a, auto b) {
    return a.coastEdge < b.coastEdge;
  });
  RiverGrowthProduct next;
  std::vector<double> heading;
  std::vector<uint64_t> mouthEdges;
  std::vector<uint32_t> candidates;
  std::set<uint64_t> usedMouths;
  for (auto m : sortedMouths)
  {
    auto found = byId.find(m.coastEdge);
    if (found == byId.end() || !usedMouths.insert(m.coastEdge).second || !std::isfinite(m.parameter) || m.parameter <= 0 || m.parameter >= 1 || m.priority == 0 || m.priority > 16)
    {
      return fail("Invalid or repeated river mouth");
    }
    auto e = found->second;
    Position p{e.a.x + (e.b.x - e.a.x) * m.parameter, land.spec.seaLevelMetres, e.a.y + (e.b.y - e.a.y) * m.parameter};
    for (auto n : next.nodes)
    {
      if (std::hypot(n.position.x - p.x, n.position.z - p.z) < s.separationMetres)
      {
        return fail("River mouths violate separation");
      }
    }
    candidates.push_back(uint32_t(next.nodes.size()));
    next.nodes.push_back({p, UINT32_MAX, m.priority, 1});
    heading.push_back(std::atan2(e.b.y - e.a.y, e.b.x - e.a.x) + 1.5707963267948966);
    mouthEdges.push_back(m.coastEdge);
  }
  auto random = [&](uint64_t node, uint64_t purpose, uint64_t sample) {
    return double(featureSeed(land.spec.seed, land.spec.generatorVersion, node + 1, purpose, sample) >> 11) / 9007199254740992.0;
  };
  while (!candidates.empty() && next.nodes.size() < s.maximumNodes)
  {
    double low = std::numeric_limits<double>::infinity();
    for (auto c : candidates)
    {
      low = std::min(low, next.nodes[c].position.y);
    }
    size_t selected = 0;
    bool found = false;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
      auto &n = next.nodes[candidates[i]];
      if (n.position.y > low + s.elevationWindowMetres)
      {
        continue;
      }
      auto &best = next.nodes[candidates[selected]];
      if (!found || n.growthPriority > best.growthPriority || (n.growthPriority == best.growthPriority && (n.position.y < best.position.y || (n.position.y == best.position.y && candidates[i] < candidates[selected]))))
      {
        selected = i;
        found = true;
      }
    }
    uint32_t parent = candidates[selected];
    candidates.erase(candidates.begin() + selected);
    auto base = next.nodes[parent];
    uint32_t rule;
    double draw = random(parent, 0x52554c45, 0);
    if (base.growthPriority == 1)
    {
      rule = 3;
    }
    else if (draw < s.continuationProbability)
    {
      rule = 0;
    }
    else if (draw < s.continuationProbability + s.symmetricProbability)
    {
      rule = 1;
    }
    else
    {
      rule = 2;
    }
    ++next.productions[rule];
    std::vector<uint32_t> priorities;
    if (rule == 0)
    {
      priorities = {base.growthPriority};
    }
    if (rule == 1)
    {
      priorities = {base.growthPriority - 1, base.growthPriority - 1};
    }
    if (rule == 2)
    {
      priorities = {base.growthPriority, 1 + uint32_t(random(parent, 0x4f524445, 0) * (base.growthPriority - 1))};
    }
    if (rule == 3)
    {
      priorities.assign(1 + uint32_t(random(parent, 0x46494c4c, 0) * 5), 1);
    }
    for (size_t child = 0; child < priorities.size(); ++child)
    {
      bool accepted = false;
      if (next.nodes.size() >= s.maximumNodes)
      {
        next.budgetReached = true;
        break;
      }
      for (uint32_t attempt = 0; attempt < s.attemptsPerChild; ++attempt)
      {
        ++next.attempts;
        double spread = priorities.size() == 1 ? 0 : (double(child) / (priorities.size() - 1) - .5) * 1.4;
        double angle = heading[parent] + spread + (random(parent, 0x414e474c, child * s.attemptsPerChild + attempt) - .5) * .8;
        Position p{base.position.x + s.edgeLengthMetres * std::cos(angle), 0, base.position.z + s.edgeLengthMetres * std::sin(angle)};
        auto control = relief.sample((p.x + base.position.x) * .5, (p.z + base.position.z) * .5);
        p.y = base.position.y + s.edgeLengthMetres * control.riverSlope;
        if (!std::isfinite(p.y) || p.y <= base.position.y)
        {
          return fail("River slope failed to produce finite upstream rise");
        }
        Point2 a = point(base.position), b = point(p);
        bool rejected = !inside(b) || (s.maximumElevationPerTerrainLambda > 0 && p.y - land.spec.seaLevelMetres > s.maximumElevationPerTerrainLambda * relief.sample(p.x, p.z).terrainLambda);
        for (auto e : coast)
        {
          const double clearance = s.coastClearanceMetres * (base.receiver == UINT32_MAX ? s.mouthClearanceScale : 1.);
          if (distance(b, e.a, e.b) < clearance || (s.maximumCoastalSlope > 0 && p.y - land.spec.seaLevelMetres > s.maximumCoastalSlope * distance(b, e.a, e.b)))
          {
            rejected = true;
          }
          if (!(base.receiver == UINT32_MAX && e.id == mouthEdges[parent]) && intersects(a, b, e.a, e.b))
          {
            rejected = true;
          }
        }
        if (rejected)
        {
          ++next.rejections[0];
          continue;
        }
        for (uint32_t i = 0; i < next.nodes.size(); ++i)
        {
          auto n = next.nodes[i];
          if (i != parent && std::hypot(p.x - n.position.x, p.z - n.position.z) < s.separationMetres)
          {
            rejected = true;
          }
          if (n.receiver != UINT32_MAX && i != parent && n.receiver != parent && segmentDistance(a, b, point(n.position), point(next.nodes[n.receiver].position)) < s.separationMetres)
          {
            rejected = true;
          }
        }
        if (rejected)
        {
          ++next.rejections[1];
          continue;
        }
        for (auto n : next.nodes)
        {
          if (std::abs(p.y - n.position.y) > s.maximumSlope * std::hypot(p.x - n.position.x, p.z - n.position.z))
          {
            rejected = true;
          }
        }
        if (rejected)
        {
          ++next.rejections[2];
          continue;
        }
        candidates.push_back(uint32_t(next.nodes.size()));
        next.nodes.push_back({p, parent, priorities[child], 1});
        heading.push_back(angle);
        mouthEdges.push_back(0);
        accepted = true;
        break;
      }
      if (!accepted)
      {
        ++next.exhaustedChildren;
      }
    }
  }
  next.budgetReached |= !candidates.empty();
  // Repair actual Horton-Strahler order after every geometric rejection. Growth
  // priority is retained separately as evidence of the production decisions.
  std::vector<uint32_t> highest(next.nodes.size()), counts(next.nodes.size());
  for (size_t i = next.nodes.size(); i-- > 0;)
  {
    auto &n = next.nodes[i];
    n.strahlerOrder = highest[i] ? highest[i] + (counts[i] > 1) : 1;
    if (n.receiver != UINT32_MAX)
    {
      auto p = n.receiver;
      if (n.strahlerOrder > highest[p])
      {
        highest[p] = n.strahlerOrder;
        counts[p] = 1;
      }
      else if (n.strahlerOrder == highest[p])
      {
        ++counts[p];
      }
    }
  }
  output = std::move(next);
  if (error)
  {
    error->clear();
  }
  return true;
}
} // namespace procedural::world::rewrite
